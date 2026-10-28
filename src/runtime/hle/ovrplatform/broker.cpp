#include "hle/ovrplatform/broker.h"
#include "hle/ovrplatform/identity_iap.h"
#include "common/log.h"
#include "common/meta/accounts.h"
#include "hle/ovrplatform/diagnostics.h"

namespace venture::ovrplatform {
namespace {
ErrorCode code(const venture::Json &j, std::string_view key) {
  const auto &v = venture::field(j, key);
  if (!v.is_number_integer())
    return ErrorCode::invalid_response;
  auto n = v.get<std::int32_t>();
  if (n == 0 || (n <= -20000 && n >= -20009))
    return static_cast<ErrorCode>(n);
  return ErrorCode::invalid_response;
}
class Brokered final : public Provider {
  AppIdentity app_;
  std::string key_, token_;
  User user_;
  Error identity_;
  bool ready_ = false;
  std::shared_ptr<PipeChannel> channel_;
  std::shared_ptr<HttpTransport> http_;
  std::mutex mutex_;
  std::once_flag cache_once_;
  std::unique_ptr<IapCache> cache_;
  IapCache& cache() {
    std::call_once(cache_once_, [&] {
      if (!venture::meta::valid_account_key(key_) || !valid_id(app_.app_id))
        throw std::invalid_argument("Invalid Platform cache path");
      auto root = venture::paths::data_dir();
      auto path = root / "accounts" / key_ / "platform" / app_.app_id;
      if (!venture::paths::within(root, path))
        throw std::invalid_argument("Invalid Platform cache path");
      cache_ = std::make_unique<IapCache>(path);
    });
    return *cache_;
  }
  Error reject(ErrorCode error) {
    ready_ = false;
    token_.clear();
    user_ = {};
    return failure(error);
  }
  Error accept(const venture::Json &packet) {
    if (packet.contains("error")) {
      auto error = code(packet, "error");
      return reject(error == ErrorCode::none ? ErrorCode::invalid_response
                                             : error);
    }
    if (venture::number(packet, "version") != 1 ||
        venture::str(packet, "account") != key_ ||
        venture::str(packet, "appId") != app_.app_id ||
        venture::str(packet, "package") != app_.package ||
        venture::str(packet, "binaryId") != app_.binary_id ||
        venture::number(packet, "versionCode") !=
            static_cast<std::uint64_t>(app_.version_code))
      return reject(ErrorCode::identity_mismatch);
    identity_ = failure(code(packet, "identityError"));
    token_ = venture::str(packet, "appToken");
    user_ = {venture::number(packet, "appUserId"), venture::str(packet, "alias"), {}};
    if (!identity_ &&
        (token_.empty() || !user_.id || token_.size() > 128 * 1024 ||
         token_.find('\0') != token_.npos))
      return reject(ErrorCode::invalid_response);
    ready_ = true;
    log::info("accounts",
              "Application credentials received identity={}", !identity_);
    return {};
  }
  Error validate() {
    if (!ready_)
      return failure(ErrorCode::uninitialized);
    try {
      auto reply = channel_->call(venture::Json{{"op", "validate"}});
      auto error = failure(code(reply, "error"));
      if (error) {
        token_.clear();
        user_ = {};
        ready_ = false;
      }
      return error;
    } catch (...) {
      token_.clear();
      user_ = {};
      ready_ = false;
      return failure(ErrorCode::unavailable);
    }
  }

public:
  Brokered(AppIdentity app, std::string key,
           std::shared_ptr<PipeChannel> channel,
           std::shared_ptr<HttpTransport> http)
      : app_(std::move(app)), key_(std::move(key)),
        channel_(std::move(channel)), http_(std::move(http)) {}
  void declare_app_id(std::string_view id) override {
    std::lock_guard lock(mutex_);
    app_.app_id = std::string(id);
  }
  Error prepare() override {
    std::lock_guard lock(mutex_);
    try {
      return accept(channel_->call(venture::Json{{"op", "credentials"}, {"appId", app_.app_id}}));
    } catch (...) {
      return failure(ErrorCode::unavailable);
    }
  }
  // Asked of Meta only now, when the game calls for it.
  Result entitlement(std::stop_token stop) override {
    {
      std::lock_guard lock(mutex_);
      if (stop.stop_requested())
        return {failure(ErrorCode::cancelled)};
      if (auto error = validate())
        return {error};
    }
    try {
      auto reply = channel_->call(venture::Json{{"op", "entitlement"}});
      if (stop.stop_requested()) return {failure(ErrorCode::cancelled)};
      return {failure(code(reply, "error"))};
    } catch (...) {
      return {failure(ErrorCode::unavailable)};
    }
  }
  Result logged_in_user(std::stop_token stop) override {
    std::lock_guard lock(mutex_);
    if (stop.stop_requested())
      return {failure(ErrorCode::cancelled)};
    if (auto error = validate())
      return {error};
    return {identity_, user_};
  }
  Result access_token(std::stop_token stop) override {
    std::lock_guard lock(mutex_);
    if (stop.stop_requested())
      return {failure(ErrorCode::cancelled)};
    if (auto error = validate())
      return {error};
    return {identity_, user_, identity_ ? "" : token_};
  }
  Result heartbeat(std::string_view options, std::stop_token stop) override {
    if (stop.stop_requested()) return {failure(ErrorCode::cancelled)};
    // The service token stays in the launcher; only presence and an interval
    // cross the same channel used for application credentials.
    try {
      auto reply = channel_->call(venture::Json{{"op", "presence_heartbeat"}, {"presence", options}});
      if (stop.stop_requested()) return {failure(ErrorCode::cancelled)};
      if (auto error = failure(code(reply, "error"))) return {error};
      const auto interval = venture::number(reply, "interval");
      if (!interval || interval > 86400) return {failure(ErrorCode::invalid_response)};
      return {{}, {}, std::to_string(interval)};
    } catch (...) { return {failure(ErrorCode::unavailable)}; }
  }
  HttpResponse graph_request(HttpRequest request, std::stop_token stop) override {
    std::string token;
    {
      std::lock_guard lock(mutex_);
      if (stop.stop_requested()) return {0, {}, failure(ErrorCode::cancelled)};
      if (auto error = validate()) return {0, {}, error};
      if (identity_) return {0, {}, identity_};
      token = token_;
    }
    try {
      request.bearer = token;
      request.app_id = app_.app_id;
      auto response = http_->send(request, stop);
      log::info("ovrplatform", "Meta endpoint={} http={} fields=[{}]",
                request.endpoint, response.status, response_fields(response));
      std::lock_guard lock(mutex_);
      if (stop.stop_requested()) return {0, {}, failure(ErrorCode::cancelled)};
      if (auto error = validate()) return {0, {}, error};
      if (token_ != token) return {0, {}, failure(ErrorCode::unauthenticated)};
      response.error = http_response_error(response);
      if (response.error.code == ErrorCode::unauthenticated) {
        token_.clear();
        user_ = {};
        // Refresh for subsequent calls; never replay this request, especially
        // ConsumePurchase. The launcher applies its existing refresh throttle.
        if (auto error = accept(channel_->call(venture::Json{{"op", "refresh"}})))
          ready_ = false;
      }
      return response;
    } catch (...) {
      return {0, {}, failure(ErrorCode::unavailable)};
    }
  }
  Result service(std::string_view name, const RequestArgs& args,
                 std::stop_token stop) override {
    if (name == "ovr_IAP_LaunchCheckoutFlow") return {unsupported_operation(name)};
    if (args.error) return {args.error};
    try {
      const bool purchases = name == "ovr_IAP_GetViewerPurchases" ||
                             name == "ovr_IAP_GetNextPurchaseArrayPage";
      const bool cached = name == "ovr_IAP_GetViewerPurchasesDurableCache";
      const bool consume = name == "ovr_IAP_ConsumePurchase";
      std::string account;
      if (purchases || cached || consume) {
        // Read only the account index, never the launcher's profile token.
        // This is the same real profile binding used by the direct provider's
        // cache; it is never exposed as a guest app-scoped user ID.
        auto index = venture::meta::AccountVault(venture::paths::data_dir()).index();
        if (auto selected = index.find(key_)) account = selected->account_id;
      }
      if (cached) {
        std::lock_guard lock(mutex_);
        if (stop.stop_requested()) return {failure(ErrorCode::cancelled)};
        if (auto error = validate()) return {error};
        return cache().read(account);
      }
      auto request = identity_iap_request(name, args);
      auto generation = purchases ? cache().generation() : 0;
      auto response = graph_request(request, stop);
      if (response.error) return {response.error};
      Model model;
      venture::Json body;
      try {
        body = venture::Json::parse(response.body);
        model = identity_iap_model(name, body);
        if (name == "ovr_User_Get" &&
            std::get<std::uint64_t>(model.fields.at("ID")) !=
                std::get<std::uint64_t>(args.values.at(0)))
          throw std::invalid_argument("Mismatched Platform user");
      } catch (...) {
        auto error = failure(ErrorCode::invalid_response);
        error.http_status = response.status;
        return {error};
      }
      if (purchases) cache().page(account, request.next_url, body, generation);
      else if (consume) cache().consumed(account, std::get<std::string>(args.values.at(0)));
      return {{}, {}, {}, std::move(model)};
    } catch (const std::invalid_argument&) {
      return {failure(ErrorCode::invalid_argument)};
    } catch (...) {
      return {failure(ErrorCode::unavailable)};
    }
  }
  Result user_proof(std::stop_token stop) override {
    std::lock_guard lock(mutex_);
    if (stop.stop_requested())
      return {failure(ErrorCode::cancelled)};
    if (auto error = validate())
      return {error};
    if (identity_)
      return {identity_};
    try {
      auto response =
          http_->post("/user_nonce_generate", {{"access_token", token_}}, stop);
      log::info("ovrplatform", "Meta /user_nonce_generate http={} fields=[{}]",
                response.status, response_fields(response));
      if (stop.stop_requested())
        return {failure(ErrorCode::cancelled)};
      auto body = venture::Json::parse(response.body, nullptr, false);
      auto graph_error = venture::field(body, "error");
      if (graph_error.is_null() && venture::field(body, "errors").is_array() &&
          !body["errors"].empty())
        graph_error = body["errors"][0];
      if (venture::number(graph_error, "code") == 190) {
        token_.clear();
        user_ = {};
        auto error = accept(channel_->call(venture::Json{{"op", "refresh"}}));
        if (error) {
          ready_ = false;
          return {error};
        }
        // Refresh credentials, but never replay the nonce POST (a single-use
        // write).
        auto expired = failure(ErrorCode::unauthenticated);
        expired.http_status = response.status;
        return {expired};
      }
      if (auto error = validate())
        return {error};
      if (response.error || response.status < 200 || response.status >= 300 ||
          !body.is_object() || !venture::field(body, "error").is_null() ||
          !venture::field(body, "errors").empty())
        return {failure(ErrorCode::unavailable)};
      auto nonce = venture::str(body, "nonce");
      if (nonce.empty() || nonce.find('\0') != nonce.npos)
        return {failure(ErrorCode::invalid_response)};
      return {{}, {}, std::move(nonce)};
    } catch (...) {
      return {failure(ErrorCode::unavailable)};
    }
  }
};
} // namespace
CredentialBroker::CredentialBroker(AppIdentity app, std::string key,
                                   std::shared_ptr<SecretStore> store,
                                   std::shared_ptr<HttpTransport> http)
    : app_(std::move(app)), key_(std::move(key)), store_(std::move(store)),
      http_(std::move(http)) {
  // "local": a launch with no account.
  if (key_ != "local" && !venture::meta::valid_account_key(key_))
    throw std::runtime_error("Invalid account key.");
}
Error CredentialBroker::prepare(std::stop_token stop) {
  std::lock_guard lock(mutex_);
  return prepare_locked(stop);
}
Error CredentialBroker::prepare_locked(std::stop_token stop) {
  // Never a launch gate: without an account the game still starts and its own
  // platform calls get that answer. Entitlement is never asked here, only when
  // the game itself calls ovr_Entitlement_GetIsViewerEntitled ("entitlement").
  try {
    profile_ = store_->load();
    provider_.reset();
    if (profile_.access_token.empty()) {
      identity_ = {failure(ErrorCode::unauthenticated)};
      return {};
    }
    auto provider = owned_library_provider(app_, store_, http_);
    if (auto error = provider->prepare()) {
      identity_ = {error};
      return {};
    }
    provider_ = std::move(provider);
    identity_ = provider_->access_token(stop);
    last_refresh_ = {};
    return {};
  } catch (...) {
    return failure(ErrorCode::unavailable);
  }
}
Error CredentialBroker::valid() {
  // The account the game started with must still be the one signed in.
  try {
    if (store_->load() != profile_)
      return failure(ErrorCode::identity_mismatch);
    return {};
  } catch (...) {
    return failure(ErrorCode::unauthenticated);
  }
}
venture::Json CredentialBroker::packet() const {
  return venture::Json{
      {"version", 1},
      {"account", key_},
      {"appId", app_.app_id},
      {"package", app_.package},
      {"binaryId", app_.binary_id},
      {"versionCode", app_.version_code},
      {"identityError", static_cast<int>(identity_.error.code)},
      {"appToken", identity_.error ? "" : identity_.text},
      {"appUserId", identity_.user.id},
      {"alias", identity_.user.alias}};
}
venture::Json CredentialBroker::handle(const venture::Json &request) {
  std::lock_guard lock(mutex_);
  auto error = valid();
  if (error)
    return venture::Json{{"error", static_cast<int>(error.code)}};
  auto op = venture::str(request, "op");
  if (op == "validate")
    return venture::Json{{"error", 0}};
  // The game declared its own app id at initialization (an import or a
  // development build may differ from the install plan): credentials are
  // Meta's answers for that id.
  if (op == "credentials" && request.contains("appId")) {
    auto id = venture::str(request, "appId");
    if (!valid_id(id))
      return venture::Json{{"error", static_cast<int>(ErrorCode::invalid_argument)}};
    if (id != app_.app_id) {
      app_.app_id = id;
      if (auto failed = prepare_locked({}))
        return venture::Json{{"error", static_cast<int>(failed.code)}};
    }
  }
  if (!provider_ && op != "credentials")
    return venture::Json{{"error", static_cast<int>(profile_.access_token.empty()
                                                         ? ErrorCode::unauthenticated
                                                         : identity_.error.code)}};
  if (op == "entitlement")
    return venture::Json{{"error", static_cast<int>(provider_->entitlement({}).error.code)}};
  if (op == "presence_heartbeat") {
    const auto& presence = venture::field(request, "presence");
    if (!presence.is_string() || presence.get_ref<const std::string&>().size() > 32768)
      return venture::Json{{"error", static_cast<int>(ErrorCode::invalid_argument)}};
    const auto result = provider_->heartbeat(presence.get_ref<const std::string&>(), {});
    return venture::Json{{"error", static_cast<int>(result.error.code)},
                    {"interval", result.error ? 0 : std::stoi(result.text)}};
  }
  if (op == "refresh") {
    // No retry storms even if a guest writes directly to its inherited pipe.
    auto now = std::chrono::steady_clock::now();
    if (now - last_refresh_ < std::chrono::seconds(5))
      return venture::Json{{"error", static_cast<int>(ErrorCode::unavailable)}};
    last_refresh_ = now;
    auto provider = owned_library_provider(app_, store_, http_);
    error = provider->prepare();
    if (error)
      return venture::Json{{"error", static_cast<int>(error.code)}};
    provider_ = std::move(provider);
    identity_ = provider_->access_token({});
    log::info("accounts", "Refreshed application credentials");
  } else if (op != "credentials")
    return venture::Json{{"error", static_cast<int>(ErrorCode::invalid_argument)}};
  return packet();
}
std::shared_ptr<Provider>
brokered_provider(AppIdentity app, std::string key,
                  std::shared_ptr<PipeChannel> channel,
                  std::shared_ptr<HttpTransport> http) {
  return std::make_shared<Brokered>(std::move(app), std::move(key),
                                    std::move(channel), std::move(http));
}
} // namespace venture::ovrplatform
