#include "hle/ovrplatform/horizon.h"
#include "common/log.h"
#include <algorithm>
#include <stdexcept>

namespace venture::ovrplatform {
namespace {
// Epic 6419 libil2cpp.so: PlatformClient::MakeRequest 0x74762ac builds
// HzPSDK_<Pascal(module)>_<Pascal(method)> but the Android tail call passes
// the original module/method separately. EventManager::ToPascalCase 0x7448468
// removes underscores and uppercases the first character of each component.
std::string pascal(std::string_view value) {
  std::string out;
  bool upper = true;
  for (char c : value) {
    if (c == '_') { upper = true; continue; }
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
      return {};
    out += upper && c >= 'a' && c <= 'z' ? char(c - 'a' + 'A') : c;
    upper = false;
  }
  return out;
}
std::string wire_name(std::string_view name) {
  // Meta LibOVRPlatformImpl64_1_real.dll UsersResponseMapper (RVA 0x1fbe10):
  // fields retain Graph spelling (alias, profile_url, profile_url_small),
  // matching Epic's Newtonsoft JsonProperty attributes, not the C getters.
  if (name == "ID") return "id";
  if (name == "OculusID") return "alias";
  if (name == "ImageUrl") return "profile_url";
  if (name == "SmallImageUrl") return "profile_url_small";
  std::string out;
  const auto upper = [](char c) { return c >= 'A' && c <= 'Z'; };
  const auto lower = [](char c) { return c >= 'a' && c <= 'z'; };
  for (std::size_t i = 0; i < name.size(); ++i) {
    auto c = name[i];
    if (c >= 'A' && c <= 'Z') {
      // C getters preserve acronyms (SKU, ID, URL); wire fields use sku/id/url.
      if (i && (!upper(name[i - 1]) || (i + 1 < name.size() && lower(name[i + 1])))) out += '_';
      c = char(c - 'A' + 'a');
    }
    out += c;
  }
  return out;
}
Json encode(const Model& model) {
  Json out = Json::object();
  if (model.kind == "String") return std::get<std::string>(model.fields.at("Value"));
  for (const auto& [name, value] : model.fields) {
    // Epic 6419 metadata: OrgScopedID's JsonProperty is org_scoped_id
    // (0x14654d3); Purchase.ID is String, not the deprecated C numeric getter.
    if (model.kind == "Purchase" && name == "PurchaseID") continue;
    auto key = model.kind == "OrgScopedID" && name == "ID" ? "org_scoped_id"
             : model.kind == "Purchase" && name == "PurchaseStrID" ? "id" : wire_name(name);
    // UsersResponseMapper 0x1fce90 preserves Graph's non-data members.
    // The shared model stores paging URLs as fields rather than a JSON object.
    if (name == "NextUrl" || name == "PreviousUrl") {
      auto url = std::get<std::string>(value);
      if (!url.empty()) out["paging"][name == "NextUrl" ? "next" : "previous"] = url;
      continue;
    }
    std::visit([&](const auto& v) {
      using T = std::decay_t<decltype(v)>;
      if constexpr (std::is_same_v<T, std::shared_ptr<const Model>>)
        out[key] = v ? encode(*v) : Json(nullptr);
      else if constexpr (std::is_same_v<T, Blob>)
        throw std::invalid_argument("Unmapped Horizon binary field");
      else out[key] = v;
    }, value);
  }
  if (model.kind.ends_with("Array")) {
    out["data"] = Json::array();
    for (const auto& item : model.elements) out["data"].push_back(encode(item));
  }
  return out;
}
// The C API answers launch details synchronously
// (ovr_ApplicationLifecycle_GetLaunchDetails); Horizon's
// ApplicationLifecycle.GetLaunchDetails is a request, answered from the same
// details. Les Mills Bodycombat's deep-link reader dereferences the result
// while its first scene loads.
constexpr RequestSpec launch_details_request{"ovr_ApplicationLifecycle_GetLaunchDetails", 0};
const RequestSpec* resolve(std::string_view module, std::string_view method) {
  auto service = pascal(module);
  if (service == "ApplicationLifecycle" && pascal(method) == "GetLaunchDetails") return &launch_details_request;
  if (service.empty()) return nullptr;
  if (service == "Iap") service = "IAP";
  const auto lower = [](std::string value) {
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return value;
  };
  // Horizon modules name services in the plural or singular where the C API
  // uses the other (Users/User, Entitlements/Entitlement, Achievements).
  std::vector<std::string> services{service};
  if (service.ends_with('s')) services.push_back(service.substr(0, service.size() - 1));
  else services.push_back(service + 's');
  // C exports preserve acronyms (ID, SKU); ToPascalCase only produces Id/Sku.
  for (const auto& candidate : services) {
    const auto name = lower("ovr_" + candidate + "_" + pascal(method));
    for (const auto& spec : request_specs())
      if (lower(spec.name) == name) return &spec;
  }
  return nullptr;
}
std::string text(const Json& value) {
  if (value.is_string()) return value.get<std::string>();
  if (value.is_boolean()) return value.get<bool>() ? "1" : "0";
  if (value.is_null()) return {};
  return value.dump();
}
std::uint64_t unsigned_value(const Json& value) {
  if (value.is_number_unsigned()) return value.get<std::uint64_t>();
  if (value.is_number_integer()) return static_cast<std::uint64_t>(value.get<std::int64_t>());
  if (value.is_number_float()) return static_cast<std::uint64_t>(value.get<double>());
  if (value.is_boolean()) return value.get<bool>();
  const auto s = text(value);
  if (s.empty() || s == "0") return 0;
  if (!valid_id(s)) throw std::invalid_argument("Horizon number");
  return std::stoull(s);
}
std::int64_t signed_value(const Json& value) {
  if (value.is_number_integer()) return value.get<std::int64_t>();
  if (value.is_number_float()) return static_cast<std::int64_t>(value.get<double>());
  if (value.is_string() && value.get_ref<const std::string&>().starts_with('-'))
    return -static_cast<std::int64_t>(unsigned_value(value.get<std::string>().substr(1)));
  return static_cast<std::int64_t>(unsigned_value(value));
}
// The length a count parameter takes from the value it counts.
std::uint64_t length(const Json& value) {
  if (value.is_array() || value.is_object()) return value.size();
  return value.is_string() ? value.get_ref<const std::string&>().size() : 0;
}
// Any other request: its JSON names the C declaration's parameters in
// snake_case (Users::Get's {user_id} is ovr_User_Get(userID)); a parameter the
// JSON leaves out takes its type's empty value.
RequestArgs generic_arguments(const RequestArgumentSpec& desc, const Json& data) {
  RequestArgs out;
  std::vector<int> counted(desc.arguments.size(), -1);
  for (std::size_t i = 0; i < desc.arguments.size(); ++i)
    if (const auto at = desc.arguments[i].count_index; at >= 0 && static_cast<std::size_t>(at) < counted.size())
      counted[at] = static_cast<int>(i);
  try {
    for (std::size_t i = 0; i < desc.arguments.size(); ++i) {
      const auto& d = desc.arguments[i];
      const auto found = data.find(wire_name(d.name));
      const Json value = found == data.end() ? Json(nullptr) : *found;
      const Json* counts = nullptr;
      if (counted[i] >= 0)
        if (auto it = data.find(wire_name(desc.arguments[counted[i]].name)); it != data.end()) counts = &*it;
      switch (d.kind) {
        case ArgKind::string: out.values.emplace_back(text(value)); break;
        case ArgKind::id:
        case ArgKind::size: out.values.emplace_back(counts ? length(*counts) : unsigned_value(value)); break;
        case ArgKind::i64: out.values.emplace_back(signed_value(value)); break;
        case ArgKind::u32:
          out.values.emplace_back(static_cast<std::uint32_t>(counts ? length(*counts) : signed_value(value)));
          break;
        case ArgKind::i32:
          out.values.emplace_back(static_cast<std::int32_t>(counts ? length(*counts) : signed_value(value)));
          break;
        case ArgKind::boolean:
          out.values.emplace_back(value.is_boolean() ? value.get<bool>() : signed_value(value) != 0);
          break;
        case ArgKind::enumeration:
          out.values.emplace_back(EnumValue{static_cast<std::int32_t>(value.is_number() ? signed_value(value) : 0)});
          break;
        case ArgKind::string_array: {
          std::vector<std::string> list;
          if (value.is_array()) for (const auto& item : value) list.push_back(text(item));
          out.values.emplace_back(std::move(list));
          break;
        }
        case ArgKind::id_array: {
          std::vector<Id> list;
          if (value.is_array()) for (const auto& item : value) list.push_back(unsigned_value(item));
          out.values.emplace_back(std::move(list));
          break;
        }
        case ArgKind::bytes: {
          const auto bytes = text(value);
          const auto* begin = reinterpret_cast<const std::byte*>(bytes.data());
          out.values.emplace_back(std::vector<std::byte>(begin, begin + bytes.size()));
          break;
        }
        case ArgKind::options: {
          // Option setters are Set<Field>; the JSON names the fields in
          // snake_case. Dictionaries are stored as Session::set_option does.
          OptionsSnapshot options{std::string(d.tag), {}};
          if (value.is_object())
            for (const auto& [field, item] : value.items()) {
              const auto name = pascal(field);
              if (name.empty()) continue;
              if (item.is_object())
                for (const auto& [k, v] : item.items()) options.fields[name + '\0' + k] = {"String", text(v)};
              else if (item.is_array())
                for (const auto& v : item) options.fields[name].push_back(text(v));
              else
                options.fields[name] = {text(item)};
            }
          out.values.emplace_back(std::move(options));
          break;
        }
        case ArgKind::model: {
          PageSnapshot page{std::string(d.tag), {}, {}};
          if (value.is_string()) page.next_url = value.get<std::string>();
          else if (value.is_object()) {
            const auto paging = value.contains("paging") ? value["paging"] : value;
            if (paging.is_object()) {
              page.next_url = text(paging.value("next", Json(nullptr)));
              page.previous_url = text(paging.value("previous", Json(nullptr)));
            }
          }
          out.values.emplace_back(std::move(page));
          break;
        }
        case ArgKind::pairs: {
          std::vector<std::string> pairs;
          if (value.is_object())
            for (const auto& [k, v] : value.items()) {
              pairs.push_back(k);
              pairs.push_back(text(v));
            }
          out.values.emplace_back(std::move(pairs));
          break;
        }
        case ArgKind::unsupported: out.values.emplace_back(std::uint64_t{0}); break;
      }
    }
  } catch (const std::exception&) {
    return {{}, failure(ErrorCode::invalid_argument)};
  }
  return out;
}
RequestArgs arguments(const RequestSpec& spec, const Json& data) {
  if (&spec == &launch_details_request)
    return data.empty() ? RequestArgs{} : RequestArgs{{}, failure(ErrorCode::invalid_argument)};
  // Request JSON is produced by Epic's service wrappers, e.g. Users::Get
  // 0x747b834 {user_id}, GetLoggedInUser 0x747c5b0 {}, Entitlements 0x745833c {}.
  for (const auto& desc : request_argument_specs()) {
    if (std::string_view(desc.name) != spec.name) continue;
    if (desc.arguments.empty() && data.empty()) return {};
    // Epic IAP::GetProductsBySKU 0x745faac sends {skus:[String]}. The DLL's
    // IAPResponseMapper 0x1ece30 flattens current_offer into the same product
    // fields already supplied by the shared host model (name/price/etc.).
    if (std::string_view(spec.name) == "ovr_IAP_GetProductsBySKU") {
      if (data.size() != 1 || !data.contains("skus") || !data["skus"].is_array() ||
          data["skus"].size() > 16384) return {{}, failure(ErrorCode::invalid_argument)};
      auto skus = data["skus"].get<std::vector<std::string>>();
      const auto count = static_cast<std::int32_t>(skus.size());
      return {{std::move(skus), count}};
    }
    // AuthenticatorOculus::GetLoggedInUserCallback 0x41b6b6c follows the
    // initial identity response with Users::Get before completing login.
    if (std::string_view(spec.name) == "ovr_User_Get" ||
        std::string_view(spec.name) == "ovr_User_GetOrgScopedID") {
      if (data.size() != 1 || !data.contains("user_id"))
        return {{}, failure(ErrorCode::invalid_argument)};
      auto id = data.at("user_id");
      auto text = id.is_string() ? id.get<std::string>() : id.dump();
      if (valid_id(text)) return {{static_cast<std::uint64_t>(std::stoull(text))}};
      return {{}, failure(ErrorCode::invalid_argument)};
    }
    return generic_arguments(desc, data);
  }
  return data.empty() ? RequestArgs{} : RequestArgs{{}, failure(ErrorCode::invalid_argument)};
}
}  // namespace
InitResult Horizon::setup(std::string_view app_id) { return session_.initialize(app_id); }
std::uint64_t Horizon::request(std::string_view module, std::string_view method,
                              std::int32_t version, std::string_view data,
                              std::int32_t cookie, bool subscription) {
  std::lock_guard lock(mutex_);
  // A subscription is a stream of events for its own session id. Venture's
  // services raise none of their own, so it stays open and quiet until stop().
  if (subscription) return next_session_++;
  const auto* spec = resolve(module, method);
  RequestArgs args;
  try {
    auto json = Json::parse(data.empty() ? "{}" : data);
    if (!json.is_object() || version != 1) args.error = failure(ErrorCode::invalid_argument);
    else if (spec) args = arguments(*spec, json);
    else {
      // pascal() keeps only letters and digits: identifiers, never guest data.
      log::warn("horizon", "no Platform request for {}.{}", pascal(module), pascal(method));
      args.error = failure(ErrorCode::unsupported);
    }
  } catch (...) { args.error = failure(ErrorCode::invalid_argument); }
  // Never log guest-controlled JSON, method strings, IDs, aliases or proofs.
  static constexpr RequestSpec unknown{"ovr_Horizon_Unsupported", 0};
  auto request = session_.request_host(spec ? *spec : unknown, std::move(args));
  pending_.push_back({request, global_session, cookie, spec ? spec->name : unknown.name});
  return request;
}
void Horizon::collect() {
  for (auto it = pending_.begin(); it != pending_.end();) {
    auto result = session_.take_result(it->request);
    if (!result) { ++it; continue; }
    if (!it->session) { it = pending_.erase(it); continue; }
    std::string data = "{}";
    if (!result->error) {
      try { data = encode(result->model).dump(-1, ' ', true); }
      catch (...) { result->error = failure(ErrorCode::invalid_response); }
    }
    // Meta's entitlement mapper 0x1e43d0 returns {} with status 0. The shared
    // handler supplies a real entitlement verdict; the DLL's mock is not used.
    ready_.push_back({it->session, {it->request, static_cast<std::int32_t>(result->error.code),
                                  it->cookie, result->error.message, std::move(data)}});
    const auto field = result->model.fields.find("ID");
    const auto* id = result->model.kind == "User" && field != result->model.fields.end()
                         ? std::get_if<Id>(&field->second) : nullptr;
    log::info("horizon", "{} completion status={} identity_present={}", it->operation,
              static_cast<int>(result->error.code), !result->error && id && *id != 0);
    it = pending_.erase(it);
  }
}
std::optional<HorizonMessage> Horizon::pop(std::uint64_t session) {
  std::lock_guard lock(mutex_);
  collect();
  for (auto it = ready_.begin(); it != ready_.end(); ++it) {
    if (it->session != session) continue;
    auto message = std::move(it->message);
    ready_.erase(it);
    return message;
  }
  return std::nullopt;
}
std::size_t Horizon::count(std::uint64_t session) {
  std::lock_guard lock(mutex_);
  collect();
  return std::count_if(ready_.begin(), ready_.end(), [=](const auto& r) { return r.session == session; });
}
void Horizon::stop(std::uint64_t session) {
  std::lock_guard lock(mutex_);
  collect();
  std::erase_if(ready_, [=](const auto& r) { return r.session == session; });
  // Subscriptions hold no request; a stopped session drops its completions.
  // Keep outstanding IDs until collect() can retire the shared completion.
  for (auto& p : pending_) if (p.session == session) p.session = 0;
}
}  // namespace venture::ovrplatform
