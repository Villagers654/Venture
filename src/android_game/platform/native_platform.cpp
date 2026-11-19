#include "android_game/platform/native_platform.h"

#include <cstdlib>
#include <set>
#include <unordered_map>

#include "common/log.h"
#include "cpu/native_entry.h"
#include "hle/ovrplatform/asset_service.h"
#include "hle/ovrplatform/hle.h"
#include "hle/ovrplatform/progression.h"

namespace venture::ovrplatform {
namespace {
constexpr const char* kLibrary = "libovrplatformloader.so";

void missing(hle::CallContext& ctx) {
  static std::mutex mutex;
  static std::set<hle::FunctionId> logged;
  {
    std::lock_guard lock(mutex);
    if (logged.insert(ctx.info().id).second)
      log::warn("ovrplatform", "{} is not implemented; the game gets 0", ctx.info().name);
  }
  ctx.return_x(0);
}

ErrorCode error_code(const Json& reply) {
  const auto& value = field(reply, "error");
  if (!value.is_number_integer()) return ErrorCode::invalid_response;
  return static_cast<ErrorCode>(value.get<std::int32_t>());
}
}  // namespace

BrokerIdentity broker_identity(const BrokerCall& broker) {
  BrokerIdentity result;
  if (!broker) {
    result.error = ErrorCode::unavailable;
    return result;
  }
  try {
    const auto reply = broker(Json{{"op", "identity"}});
    if (auto code = error_code(reply); code != ErrorCode::none) {
      result.error = code;
      return result;
    }
    result.app = {str(reply, "appId"), str(reply, "package"), str(reply, "binaryId"),
                  static_cast<std::int64_t>(number(reply, "versionCode")), str(reply, "versionName")};
    result.account = str(reply, "account");
  } catch (const std::exception& e) {
    log::warn("ovrplatform", "the Venture app's broker could not be reached: {}", e.what());
    result = {};
    result.error = ErrorCode::unavailable;
  }
  return result;
}

NativePlatform::NativePlatform(std::span<const char* const> names, std::function<NativePlatformConfig()> configure)
    : names_(names), configure_(std::move(configure)) {}

NativePlatform::~NativePlatform() {
  if (session_) session_->shutdown();
}

void NativePlatform::start() {
  auto config = configure_();
  identity_ = broker_identity(config.broker);
  if (identity_.error != ErrorCode::none)
    log::warn("ovrplatform", "the Venture app did not identify this game (error {}); Platform calls will fail",
              static_cast<int>(identity_.error));
  else
    log::info("ovrplatform", "serving {} (app {}) for account {}", identity_.app.package, identity_.app.app_id,
              identity_.account);
  BrokerCall broker = config.broker;
  if (!broker || identity_.error != ErrorCode::none) {
    const auto error = identity_.error == ErrorCode::none ? ErrorCode::unavailable : identity_.error;
    broker = [error](const Json&) { return Json{{"error", static_cast<int>(error)}}; };
  }
  auto provider = brokered_provider(identity_.app, identity_.account, broker, config.http);
  // Guest and host share the address space: handles are ordinary allocations.
  Memory memory{
      [](std::size_t size) { return static_cast<Handle>(reinterpret_cast<std::uintptr_t>(std::calloc(1, size))); },
      [](Handle address) { std::free(reinterpret_cast<void*>(static_cast<std::uintptr_t>(address))); },
  };
  session_ = std::make_unique<Session>(identity_.app, provider, std::move(memory));
  register_progression_handlers(*session_);
  // Asset files: the catalog the Venture app copied beside the OBBs. Without
  // it (or for another build's) the asset calls stay unanswered, as the HLE
  // leaves them without an install.
  if (std::error_code ec; !config.obb_dir.empty() && std::filesystem::is_regular_file(config.obb_dir / "plan.json", ec)) {
    try {
      register_asset_handlers(
          *session_, std::make_shared<AssetService>(identity_.app, config.obb_dir, config.obb_dir.string(),
                                                    asset_ownership_reader(provider, config.http),
                                                    AssetLauncherCall(broker), config.language_state));
    } catch (const std::exception& e) {
      log::warn("ovrplatform", "asset files are unavailable: {}", e.what());
    }
  }
  if (!config.cloud_dir.empty())
    register_cloud_storage_handler(*session_, config.cloud_dir.string(), config.cloud_dir);

  std::unordered_map<std::string_view, hle::Handler> handlers;
  for (const auto& entry : exports()) handlers.emplace(entry.name, entry.handler);
  for (const auto& entry : config.extra_exports) handlers.insert_or_assign(entry.name, entry.handler);
  functions_.resize(names_.size());
  for (std::size_t id = 1; id < names_.size(); ++id) {
    auto& info = functions_[id];
    const auto found = handlers.find(names_[id]);
    info.handler = found != handlers.end() ? found->second : missing;
    implemented_ += found != handlers.end();
    info.user = session_.get();
    info.library = kLibrary;
    info.name = names_[id];
    info.id = static_cast<hle::FunctionId>(id);
  }
  log::info("ovrplatform", "{} of {} exports implemented", implemented_, names_.size() - 1);
}

std::uint32_t NativePlatform::dispatch(NativeContext& context) noexcept {
  const auto id = context.x[16];
  if (id == 0 || id >= names_.size()) {
    context.x[0] = 0;
    return VENTURE_NATIVE_EXIT_NONE;
  }
  try {
    std::call_once(started_, [this] { start(); });
    NativeContextCpu cpu(context);
    hle::CallContext call(cpu, functions_[id], hle::Registers(context.x));
    functions_[id].handler(call);
  } catch (const std::exception& e) {
    log::error("ovrplatform", "{} failed: {}", names_[id], e.what());
    context.x[0] = 0;
  } catch (...) {
    log::error("ovrplatform", "{} failed", names_[id]);
    context.x[0] = 0;
  }
  return VENTURE_NATIVE_EXIT_NONE;
}
}  // namespace venture::ovrplatform
