#include "hle/ovrplatform/hle.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <mutex>

#include "common/log.h"
#include "common/environment.h"
#include "core/process.h"
#include "common/milestone.h"

namespace venture::ovrplatform {
namespace {
Session& session(hle::CallContext& ctx) { return *static_cast<Session*>(ctx.user()); }
std::string_view text(GuestAddr address) {
  return address ? std::string_view(g2h<const char>(address)) : std::string_view{};
}
void log_init(hle::CallContext& ctx) {
  milestone::mark("platform_init");
  const auto app = text(ctx.x(0));
  log::info("ovrplatform", "{} guest_app_id={} caller={:#x}", ctx.info().name,
            valid_id(app) ? app : "<invalid>", ctx.cpu().lr());
}
void sync_init(hle::CallContext& ctx) {
  // r5 V-Speedway/Bait: Wrapper at 0x53570 tail-calls Internal, passing
  // the same app ID and JNI contexts. Both return the Unity boolean result.
  log_init(ctx);
  auto result = session(ctx).initialize(text(ctx.x(0)));
  const bool unity = ctx.info().name == "ovr_UnityInitWrapper" ||
                     ctx.info().name == "ovr_UnityInitWrapperInternal";
  ctx.return_x(unity ? result == InitResult::success : static_cast<std::uint64_t>(result));
}
void async_init(hle::CallContext& ctx) {
  log_init(ctx);
  ctx.return_x(session(ctx).initialize_async(text(ctx.x(0))));
}
void sync_options(hle::CallContext& ctx) {
  // Android WithOptions preserves X0..X4. Verified in the V-Speedway/Bait
  // loader at VA 0x537f0 (r5-platform evidence); X3/X4 are the config
  // array/count. Nonempty configuration is not silently discarded.
  if (ctx.x(4)) {
    log_init(ctx);
    log::warn("ovrplatform", "{}: initialization options unsupported", ctx.info().name);
    ctx.return_x(static_cast<std::uint64_t>(InitResult::unknown));
  } else
    sync_init(ctx);
}
void async_options(hle::CallContext& ctx) {
  log_init(ctx);
  ctx.return_x(ctx.x(4) ? session(ctx).reject_initialization(0x1ad307b4)
                        : session(ctx).initialize_async(text(ctx.x(0))));
}
void reject_developer_token(hle::CallContext& ctx) {
  log::warn("ovrplatform", "{}: developer credential scope is not verified", ctx.info().name);
  ctx.return_x(0);  // SDK bool; never substitute for the signed-in account.
}
void reject_standalone(hle::CallContext& ctx) {
  ctx.return_x(session(ctx).reject_initialization(0x51f8ce0c));
}
void pop(hle::CallContext& ctx) { ctx.return_x(session(ctx).pop()); }
void free_message(hle::CallContext& ctx) { session(ctx).free_message(ctx.x(0)); }
void initialized(hle::CallContext& ctx) { ctx.return_x(session(ctx).initialized()); }
void user_id(hle::CallContext& ctx) {
  const auto id = session(ctx).logged_in_user_id();
  log::info("ovrplatform", "{} caller={:#x} identity_returned={}", ctx.info().name, ctx.cpu().lr(),
            id != 0);
  ctx.return_x(id);
}
void getter(hle::CallContext& ctx) {
  auto bits = session(ctx).get(ctx.x(0), ctx.info().name, ctx.x(1));
  static const bool trace_optional = venture::environment::getenv("VENTURE_PLATFORM_TRACE_OPTIONAL_STRINGS") != nullptr;
  if (trace_optional && (ctx.info().name == "ovr_User_GetImageUrl" ||
                         ctx.info().name == "ovr_User_GetSmallImageUrl" ||
                         ctx.info().name == "ovr_Message_GetUser"))
    log::info("platform-optional", "{} input_null={} output_null={}", ctx.info().name,
              ctx.x(0) == 0, bits == 0);
  if (ctx.info().name == "ovr_UserAccountAgeCategory_GetAgeCategory")
    log::info("ovrplatform", "User age category getter enum={}", bits);
  auto* d = model_getter(ctx.info().name);
  if (d && d->type == GetterType::f32)
    ctx.return_s(std::bit_cast<float>(static_cast<std::uint32_t>(bits)));
  else if (d && d->type == GetterType::f64)
    ctx.return_d(std::bit_cast<double>(bits));
  else
    ctx.return_x(bits);
}
void launch_details(hle::CallContext& ctx) { ctx.return_x(session(ctx).launch_details()); }
void request(hle::CallContext& ctx) {
  for (const auto& spec : request_specs())
    if (spec.name == ctx.info().name) {
      auto found = std::find_if(request_argument_specs().begin(), request_argument_specs().end(),
                                [&](const auto& args) { return args.name == ctx.info().name; });
      hle::ArgCursor cursor(ctx);
      std::vector<std::uint64_t> bits;
      for (const auto& d : found->arguments) {
        (void)d;
        bits.push_back(cursor.next_gpr());
      }
      ctx.return_x(
          session(ctx).request(spec, snapshot_request_args(session(ctx), *found, bits, 8)));
      return;
    }
}
struct OptionSpec {
  const char *name, *kind, *action, *types;
};
constexpr OptionSpec option_specs[] = {
#define VENTURE_OPTION(name, kind, action, types) {name, kind, action, types},
#include "hle/ovrplatform/options.inc"
#undef VENTURE_OPTION
};
void option(hle::CallContext& ctx) {
  const auto* spec = std::find_if(std::begin(option_specs), std::end(option_specs),
                                  [&](const auto& spec) { return spec.name == ctx.info().name; });
  if (spec == std::end(option_specs)) return;
  const std::string_view action = spec->action;
  if (action == "Create") {
    ctx.return_x(session(ctx).create_options(spec->kind));
    return;
  }
  if (action == "Destroy") {
    session(ctx).destroy_options(ctx.x(0), spec->kind);
    return;
  }
  if (action.starts_with("Clear")) {
    session(ctx).clear_options(ctx.x(0), spec->kind, action.substr(5));
    return;
  }
  hle::ArgCursor cursor(ctx);
  auto handle = cursor.next_gpr();
  std::vector<std::string> values;
  for (const char* type = spec->types + 1; *type; ++type) {
    if (*type == 's')
      values.emplace_back(text(cursor.next_gpr()));
    else if (*type == 'd')
      values.push_back(std::format("{:.17g}", cursor.next_double()));
    else if (*type == 'f')
      values.push_back(std::format("{:.9g}", cursor.next_float()));
    else if (*type == 'b')
      values.push_back((cursor.next_gpr() & 0xff) ? "1" : "0");
    else if (*type == 'i')
      values.push_back(std::to_string(static_cast<std::int32_t>(cursor.next_gpr())));
    else if (*type == 'u')
      values.push_back(std::to_string(static_cast<std::uint32_t>(cursor.next_gpr())));
    else
      values.push_back(std::to_string(cursor.next_gpr()));
  }
  session(ctx).set_option(handle, spec->kind, action.substr(3), std::move(values),
                          action.starts_with("Add"));
}
void no_data(hle::CallContext& ctx) {
  // Defined no-data/false states, not completed services: no packets, no voice
  // connections and no remote PCM exist without a real provider.
  ctx.return_x(0);
}
void microphone_mute(hle::CallContext& ctx) {
  // OVR_Functions_Voip.h: void(ovrVoipMuteState), safe on any calling thread.
  session(ctx).set_microphone_mute(static_cast<VoipMuteState>(static_cast<std::int32_t>(ctx.x(0))));
}
void no_voice_connection(hle::CallContext&) {
  // The device has no voice peers or transport. Accept/Start/Stop cannot
  // create a connection or post a connection-established notification, so
  // options, output rate and microphone filters for connections never apply.
}
void voice_status(hle::CallContext& ctx) {
  ctx.return_x(static_cast<std::uint32_t>(SystemVoipStatus::unavailable));
}
void send_failed(hle::CallContext& ctx) {
  log::warn("ovrplatform", "{}: packet networking unavailable", ctx.info().name);
  ctx.return_x(0);
}
void http_failed(hle::CallContext& ctx) {
  // CAPI.cs: caller selects the completion message type. Do not fetch a guest
  // URL with the account credential, and do not leave the request unfinished.
  auto type = static_cast<std::uint32_t>(ctx.x(1));
  ctx.return_x(session(ctx).request({"ovr_HTTP_GetWithMessageType", type}));
}
void log_new_event(hle::CallContext&) {
  // Local diagnostic receipt only. Event names/values can contain credentials.
  log::debug("ovrplatform", "Platform local analytics event received");
}
void log_deeplink_result(hle::CallContext&) {
  // OVR_Functions_ApplicationLifecycle.h: void (trackingID, result). Receipt
  // is local only; the tracking ID and guest result are deliberately omitted.
  log::debug("ovrplatform", "Platform deeplink result received");
}
void locale(hle::CallContext& ctx) { ctx.return_x(session(ctx).locale()); }
void message_type_string(hle::CallContext& ctx) {
  ctx.return_x(session(ctx).message_type_string(static_cast<std::uint32_t>(ctx.x(0))));
}
constexpr std::string_view launch_types[]{"Unknown", "Normal", "Invite", "Coordinated", "Deeplink"};
void launch_type_string(hle::CallContext& ctx) {
  const auto value = static_cast<std::uint32_t>(ctx.x(0));
  ctx.return_x(session(ctx).enum_string(launch_types[value < std::size(launch_types) ? value : 0]));
}
void launch_type_from_string(hle::CallContext& ctx) {
  const auto value = text(ctx.x(0));
  const auto found = std::find(std::begin(launch_types), std::end(launch_types), value);
  ctx.return_x(found == std::end(launch_types) ? 0 : found - std::begin(launch_types));
}
// ovr<Enum>_ToString/_FromString (OVR_<Enum>.h) for every SDK enum: the
// guest loader forwards them to the platform library, which is this one.
struct EnumString {
  std::string_view type;
  std::int32_t value;
  std::string_view text;
};
constexpr EnumString enum_strings[]{
#define VENTURE_ENUM_TYPE(type)
#define VENTURE_ENUM_STRING(type, value, text) {type, value, text},
#include "hle/ovrplatform/enum_strings.inc"
#undef VENTURE_ENUM_STRING
#undef VENTURE_ENUM_TYPE
};
std::string_view enum_type(std::string_view function) { return function.substr(0, function.rfind('_')); }
void enum_to_string(hle::CallContext& ctx) {
  const auto type = enum_type(ctx.info().name);
  const auto value = static_cast<std::int32_t>(ctx.x(0));
  std::string_view found, unknown;
  for (const auto& e : enum_strings)
    if (e.type == type) {
      if (e.value == value) found = e.text;
      if (e.text == "UNKNOWN") unknown = e.text;
    }
  ctx.return_x(session(ctx).enum_string(found.empty() ? unknown : found));
}
void enum_from_string(hle::CallContext& ctx) {
  const auto type = enum_type(ctx.info().name);
  const auto value = ctx.x(0) ? text(ctx.x(0)) : std::string_view();
  std::int32_t found = 0, unknown = 0;
  bool matched = false;
  for (const auto& e : enum_strings)
    if (e.type == type) {
      if (e.text == "UNKNOWN") unknown = e.value;
      if (!matched && e.text == value) found = e.value, matched = true;
    }
  ctx.return_x(static_cast<std::uint32_t>(matched ? found : unknown));
}
void id_to_string(hle::CallContext& ctx) {
  // OVR_Types.h: bool (char* out, size_t length, ovrID id).
  char buffer[21];
  auto [end, error] = std::to_chars(buffer, buffer + 20, ctx.x(2));
  const auto length = static_cast<std::uint64_t>(end - buffer);
  bool ok = ctx.x(0) && error == std::errc{} && ctx.x(1) > length;
  if (ok) {
    std::memcpy(g2h(ctx.x(0)), buffer, length);
    g2h<char>(ctx.x(0))[length] = '\0';
  }
  ctx.return_x(ok);
}
void id_from_string(hle::CallContext& ctx) {
  auto s = text(ctx.x(1));
  Id id{};
  if (s.empty()) {
    ctx.return_x(0);
    return;
  }
  auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), id);
  bool ok = ctx.x(0) && !s.empty() && error == std::errc{} && end == s.data() + s.size();
  if (ok) std::memcpy(g2h(ctx.x(0)), &id, sizeof(id));
  ctx.return_x(ok);
}
void key_value_string(hle::CallContext& ctx) {
  // OVR_Types.h helper returns a by-value struct; String = 0.
  KeyValuePair64 value{ctx.x(0), 0, 0, ctx.x(1), 0, 0, 0};
  std::memcpy(g2h(ctx.x(8)), &value, sizeof(value));
}
void key_value_int(hle::CallContext& ctx) {
  KeyValuePair64 value{ctx.x(0), 1, 0, 0, static_cast<std::int32_t>(ctx.x(1)), 0, 0};
  std::memcpy(g2h(ctx.x(8)), &value, sizeof(value));
}
void key_value_double(hle::CallContext& ctx) {
  KeyValuePair64 value{ctx.x(0), 2, 0, 0, 0, 0, ctx.d(0)};
  std::memcpy(g2h(ctx.x(8)), &value, sizeof(value));
}
// Only functions with implemented local semantics or a verified asynchronous
// failure contract are listed. Everything else uses the core missing thunk.
const std::vector<Export>& table() {
  static const std::vector<Export> result = {
#define VENTURE_EXPORT(name, handler) {name, handler}
      VENTURE_EXPORT("ovr_PlatformInitializeAndroid", sync_init),
      VENTURE_EXPORT("ovr_PlatformInitializeAndroidAsynchronous", async_init),
      VENTURE_EXPORT("ovr_PlatformInitializeAndroidWithOptions", sync_options),
      VENTURE_EXPORT("ovr_PlatformInitializeAndroidAsynchronousWithOptions", async_options),
      VENTURE_EXPORT("ovr_UnityInitWrapper", sync_init),
      VENTURE_EXPORT("ovr_UnityInitWrapperAsynchronous", async_init),
      VENTURE_EXPORT("ovr_UnityInitWrapperInternal", sync_init),
      VENTURE_EXPORT("ovr_UnityInitWrapperInternalAsynchronous", async_init),
      VENTURE_EXPORT("ovr_UnityInitWrapperStandalone", reject_developer_token),
      VENTURE_EXPORT("ovr_Platform_InitializeStandaloneOculus", reject_standalone),
      VENTURE_EXPORT("ovr_SetDeveloperAccessToken", reject_developer_token),
      VENTURE_EXPORT("ovr_IsPlatformInitialized", initialized),
      VENTURE_EXPORT("ovr_GetLoggedInUserID", user_id),
      VENTURE_EXPORT("ovr_Log_NewEvent", log_new_event),
      VENTURE_EXPORT("ovr_Log_Event", log_new_event),
      VENTURE_EXPORT("ovr_Log_NewUnifiedEvent", log_new_event),
      VENTURE_EXPORT("ovr_Log_UnifiedEvent", log_new_event),
      VENTURE_EXPORT("ovr_GetLoggedInUserLocale", locale),
      VENTURE_EXPORT("ovrMessageType_ToString", message_type_string),
      VENTURE_EXPORT("ovrLaunchType_ToString", launch_type_string),
      VENTURE_EXPORT("ovrLaunchType_FromString", launch_type_from_string),
      VENTURE_EXPORT("ovrID_FromString", id_from_string),
      VENTURE_EXPORT("ovrID_ToString", id_to_string),
      VENTURE_EXPORT("ovrKeyValuePair_makeString", key_value_string),
      VENTURE_EXPORT("ovrKeyValuePair_makeInt", key_value_int),
      VENTURE_EXPORT("ovrKeyValuePair_makeDouble", key_value_double),
      VENTURE_EXPORT("ovr_Voip_GetSystemVoipStatus", voice_status),
      VENTURE_EXPORT("ovr_Voip_SetMicrophoneMuted", microphone_mute),
      VENTURE_EXPORT("ovr_Voip_Accept", no_voice_connection),
      VENTURE_EXPORT("ovr_Voip_Start", no_voice_connection),
      VENTURE_EXPORT("ovr_Voip_Stop", no_voice_connection),
      // OVR_Functions_Voip.h: void setters for future connections and capture.
      VENTURE_EXPORT("ovr_Voip_SetNewConnectionOptions", no_voice_connection),
      VENTURE_EXPORT("ovr_Voip_SetOutputSampleRate", no_voice_connection),
      VENTURE_EXPORT("ovr_Voip_SetMicrophoneFilterCallback", no_voice_connection),
      VENTURE_EXPORT("ovr_Voip_SetMicrophoneFilterCallbackWithFixedSizeBuffer", no_voice_connection),
      // Per-peer queries: no DTX, ovrVoipBitrate_Unknown and no timestamp offset.
      VENTURE_EXPORT("ovr_Voip_GetIsConnectionUsingDtx", no_data),
      VENTURE_EXPORT("ovr_Voip_GetLocalBitrate", no_data),
      VENTURE_EXPORT("ovr_Voip_GetSyncTimestampDifference", no_data),
      // OVR_Functions_Voip.h: capacity in PCM elements. No rings are allocated
      // on this zero-connection device, so no samples can ever be returned.
      VENTURE_EXPORT("ovr_Voip_GetOutputBufferMaxSize", no_data),
      VENTURE_EXPORT("ovr_PopMessage", pop),
      VENTURE_EXPORT("ovr_FreeMessage", free_message),
      VENTURE_EXPORT("ovr_Error_GetCode", getter),
      VENTURE_EXPORT("ovr_Error_GetHttpCode", getter),
      VENTURE_EXPORT("ovr_Error_GetMessage", getter),
      VENTURE_EXPORT("ovr_Error_GetDisplayableMessage", getter),
      VENTURE_EXPORT("ovr_ApplicationLifecycle_GetLaunchDetails", launch_details),
      VENTURE_EXPORT("ovr_ApplicationLifecycle_LogDeeplinkResult", log_deeplink_result),
      VENTURE_EXPORT("ovr_Net_ReadPacket", no_data),
      VENTURE_EXPORT("ovr_Net_IsConnected", no_data),
      VENTURE_EXPORT("ovr_HTTP_GetWithMessageType", http_failed),
      VENTURE_EXPORT("ovr_Net_SendPacket", send_failed),
      VENTURE_EXPORT("ovr_Net_SendPacketToCurrentRoom", send_failed),
      VENTURE_EXPORT("ovr_Voip_GetPCM", no_data),
      VENTURE_EXPORT("ovr_Voip_GetPCMFloat", no_data),
      VENTURE_EXPORT("ovr_Voip_GetPCMSize", no_data),
      VENTURE_EXPORT("ovr_Voip_GetPCMWithTimestamp", no_data),
      VENTURE_EXPORT("ovr_Voip_GetPCMWithTimestampFloat", no_data),
      VENTURE_EXPORT("ovr_Voip_GetRemoteBitrate", no_data),
      VENTURE_EXPORT("ovr_Voip_GetSyncTimestamp", no_data),
      VENTURE_EXPORT("ovr_NetSync_GetAmbisonicFloatPCM", no_data),
      VENTURE_EXPORT("ovr_NetSync_GetAmbisonicInt16PCM", no_data),
      VENTURE_EXPORT("ovr_NetSync_GetAmbisonicInterleavedFloatPCM", no_data),
      VENTURE_EXPORT("ovr_NetSync_GetAmbisonicInterleavedInt16PCM", no_data),
      VENTURE_EXPORT("ovr_NetSync_GetMonostreamFloatPCM", no_data),
      VENTURE_EXPORT("ovr_NetSync_GetMonostreamInt16PCM", no_data),
      VENTURE_EXPORT("ovr_NetSync_GetListenerPosition", no_data),
      VENTURE_EXPORT("ovr_NetSync_GetVoipAmplitude", no_data),
#undef VENTURE_EXPORT
#define VENTURE_REQUEST(name, type) {name, request},
#include "hle/ovrplatform/requests.inc"
#undef VENTURE_REQUEST
#define VENTURE_OPTION(name, kind, action, types) {name, option},
#include "hle/ovrplatform/options.inc"
#undef VENTURE_OPTION
#define VENTURE_MODEL_GETTER(name, model, field, type, op, nested) {name, getter},
#include "hle/ovrplatform/model_getters.inc"
#undef VENTURE_MODEL_GETTER
#define VENTURE_GETTER(name) {name, getter},
#include "hle/ovrplatform/message_getters.inc"
#undef VENTURE_GETTER
#define VENTURE_ENUM_TYPE(type) {type "_ToString", enum_to_string}, {type "_FromString", enum_from_string},
#define VENTURE_ENUM_STRING(type, value, text)
#include "hle/ovrplatform/enum_strings.inc"
#undef VENTURE_ENUM_STRING
#undef VENTURE_ENUM_TYPE
  };
  return result;
}
}  // namespace
std::span<const Export> exports() { return table(); }

namespace {
void platform32(hle::CallContext& ctx) {
  using namespace hle::a32;
  try {
    const auto& d = *ctx.info().a32;
    auto found = std::find_if(table().begin(), table().end(),
                              [&](const Export& e) { return e.name == d.symbol; });
    if (found == table().end()) throw std::logic_error("A32 Platform export missing");
    auto fn = found->handler;
    ArgCursor32 cursor(ctx.cpu());
    ResultWriter32 result(ctx.cpu());
    std::uint64_t value = 0;
    if (fn == async_init || fn == async_options) {
      auto app = cursor.word();
      cursor.word();
      cursor.word();
      bool options = false;
      if (fn == async_options) {
        cursor.word();
        options = cursor.word() != 0;
      }
      log_init(ctx);
      value = options ? session(ctx).reject_initialization(0x1ad307b4)
                      : session(ctx).initialize_async(text(app));
    } else if (fn == sync_options) {
      auto app = cursor.word();
      cursor.word();
      cursor.word();
      cursor.word();
      if (cursor.word()) {
        log::warn("ovrplatform", "{}: initialization options unsupported", d.symbol);
        value = std::uint64_t(InitResult::unknown);
      } else
        value = std::uint64_t(session(ctx).initialize(text(app)));
    } else if (fn == request) {
      for (const auto& spec : request_specs())
        if (spec.name == d.symbol) {
          auto found =
              std::find_if(request_argument_specs().begin(), request_argument_specs().end(),
                           [&](const auto& args) { return args.name == d.symbol; });
          std::vector<std::uint64_t> bits;
          for (const auto& arg : found->arguments) {
            if (arg.kind == ArgKind::id || arg.kind == ArgKind::i64)
              bits.push_back(cursor.pair());
            else
              bits.push_back(cursor.word());
          }
          value = session(ctx).request(spec, snapshot_request_args(session(ctx), *found, bits, 4));
          break;
        }
    } else if (fn == user_id)
      value = session(ctx).logged_in_user_id();
    else if (fn == getter) {
      auto handle = cursor.word();
      auto index = d.arguments.size() > 1 ? cursor.word() : 0;
      value = session(ctx).get(handle, d.symbol, index);
    } else if (fn == pop)
      value = session(ctx).pop();
    else if (fn == launch_details)
      value = session(ctx).launch_details();
    else if (fn == http_failed) {
      cursor.word();
      value = session(ctx).request({"ovr_HTTP_GetWithMessageType", cursor.word()});
    } else if (fn == reject_standalone)
      value = session(ctx).reject_initialization(0x51f8ce0c);
    else if (fn == option) {
      auto spec = std::find_if(std::begin(option_specs), std::end(option_specs),
                               [&](auto& s) { return s.name == d.symbol; });
      const std::string_view action = spec->action;
      if (action == "Create")
        value = session(ctx).create_options(spec->kind);
      else {
        auto handle = cursor.word();
        if (action == "Destroy")
          session(ctx).destroy_options(handle, spec->kind);
        else if (action.starts_with("Clear"))
          session(ctx).clear_options(handle, spec->kind, action.substr(5));
        else {
          std::vector<std::string> values;
          for (std::size_t i = 1; i < d.arguments.size(); ++i) {
            auto bits = cursor.next(d.arguments[i]);
            auto type = spec->types[i];
            if (type == 's')
              values.emplace_back(text(bits));
            else if (type == 'd')
              values.push_back(std::format("{:.17g}", std::bit_cast<double>(bits)));
            else if (type == 'f')
              values.push_back(std::format("{:.9g}", std::bit_cast<float>(std::uint32_t(bits))));
            else if (type == 'i')
              values.push_back(std::to_string(std::int32_t(bits)));
            else if (type == 'b')
              values.push_back(bits ? "1" : "0");
            else
              values.push_back(std::to_string(bits));
          }
          session(ctx).set_option(handle, spec->kind, action.substr(3), std::move(values),
                                  action.starts_with("Add"));
        }
      }
    } else if (fn == key_value_string) {
      // ARM base PCS: hidden result@r0; key@r1, string@r2.
      struct Pair32 {
        std::uint32_t key;
        std::int32_t type;
        std::uint32_t string;
        std::int32_t integer;
        double number;
      };
      static_assert(sizeof(Pair32) == 24 && offsetof(Pair32, number) == 16);
      Pair32 p{std::uint32_t(ctx.x(1)), 0, std::uint32_t(ctx.x(2)), 0, 0};
      result.composite(std::as_bytes(std::span(&p, 1)));
      return;
    } else {
      // These handlers only inspect the first three word arguments, or no
      // arguments. No host object is exposed: Session allocates its opaque
      // HandleCells and strings in AddressSpace's low arena for an A32 process.
      fn(ctx);
      value = ctx.x(0);
    }
    if (d.result.kind == Kind::Handle)
      result.scalar({Kind::Ptr}, value);
    else
      result.scalar(d.result, value);
  } catch (const hle::a32::AbiError& e) {
    hle::a32::fail(ctx, e.error);
  }
}
}  // namespace
void register_a32_adapters() {
  for (const auto& e : exports())
    if (hle::a32::descriptor("libovrplatformloader.so", e.name))
      hle::a32::register_adapter("libovrplatformloader.so", e.name, platform32);
}
}  // namespace venture::ovrplatform
