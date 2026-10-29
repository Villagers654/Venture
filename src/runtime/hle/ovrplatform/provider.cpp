#include "hle/ovrplatform/provider.h"

#include <algorithm>
#include <charconv>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <optional>
#include <nlohmann/json.hpp>
#include <stdexcept>

#include "common/log.h"
#include "hle/ovrplatform/diagnostics.h"
#include "hle/ovrplatform/identity_iap.h"

namespace venture::ovrplatform {
namespace {
using Json = nlohmann::json;
std::string string(const Json& j, const char* key) {
  auto it = j.find(key);
  if (it == j.end()) return {};
  if (it->is_string()) return it->get<std::string>();
  if (it->is_number_unsigned()) return std::to_string(it->get<std::uint64_t>());
  return {};
}
Error response_error(const HttpResponse& response, Json& body) {
  if (auto err = http_response_error(response)) return err;
  body = Json::parse(response.body, nullptr, false);
  return {};
}
class OwnedLibrary final : public Provider {
 public:
  OwnedLibrary(AppIdentity app, LaunchCredentials stores,
               std::shared_ptr<HttpTransport> http)
      : app_(std::move(app)),
        store_(std::move(stores.identity_store)),
        entitlement_store_(std::move(stores.entitlement_store)),
        http_(std::move(http)) {}
  Error prepare() override {
    // An imported build has no Meta binary id; the app id is what Meta answers for.
    if (!valid_id(app_.app_id) || app_.package.empty() ||
        (!app_.binary_id.empty() && !valid_id(app_.binary_id)) || app_.version_code <= 0)
      return failure(ErrorCode::identity_mismatch);
    try {
      auto current = store_->load();
      if (current.access_token.empty()) return failure(ErrorCode::unauthenticated);
      auto entitlement = entitlement_store_->load();
      if (entitlement.access_token.empty())
        return failure(ErrorCode::unauthenticated);
      std::lock_guard lock(mutex_);
      if (!credential_.access_token.empty() && current != credential_)
        return failure(ErrorCode::identity_mismatch);
      if (!entitlement_credential_.access_token.empty() &&
          entitlement != entitlement_credential_)
        return failure(ErrorCode::identity_mismatch);
      credential_ = std::move(current);
      entitlement_credential_ = std::move(entitlement);
      return {};
    } catch (...) {
      return failure(ErrorCode::unauthenticated);
    }
  }
  void declare_app_id(std::string_view id) override {
    std::lock_guard lock(mutex_);
    if (app_.app_id == id) return;
    app_.app_id = std::string(id);
    // An application token belongs to the app id it was issued for.
    ++auth_epoch_;
    app_token_.clear();
    app_user_ = {};
    auth_error_ = {};
  }
  Result entitlement(std::stop_token stop) override {
    Credential credential;
    {
      std::lock_guard lock(mutex_);
      credential = entitlement_credential_;
    }
    if (credential.access_token.empty()) return {failure(ErrorCode::uninitialized)};
    try {
      if (entitlement_store_->load() != credential)
        return {failure(ErrorCode::identity_mismatch)};
      // Same live active-entitlements query as tools/meta_cli.py: no cache and
      // no inference from downloaded files. r5-platform/report.md provider
      // split.
      auto response = http_->post("/graphql",
                                  {{"access_token", credential.access_token},
                                   {"doc_id", "4850747515044496"},
                                   {"variables", "{}"}},
                                  stop);
      if (stop.stop_requested()) return {failure(ErrorCode::cancelled)};
      if (entitlement_store_->load() != credential)
        return {failure(ErrorCode::identity_mismatch)};
      Json body;
      if (auto error = response_error(response, body)) {
        if (error.code == ErrorCode::unauthenticated) invalidate_auth();
        // Meta fails the whole-library query for some accounts (HTTP 500 on
        // every try); the store listing answers for this app alone.
        if (error.code == ErrorCode::unavailable)
          if (auto answer = item_entitlement(credential, stop)) return std::move(*answer);
        return {std::move(error)};
      }
      const auto& viewer = body.at("data").at("viewer");
      const auto& user = viewer.at("user");
      auto user_id = string(user, "id");
      if (!valid_id(user_id)) return {failure(ErrorCode::invalid_response)};
      if (!credential.account_id.empty() && credential.account_id != user_id)
        return {failure(ErrorCode::identity_mismatch)};
      const auto& connection = user.contains("active_entitlements")
                                   ? user.at("active_entitlements")
                                   : viewer.at("active_entitlements");
      const Json* nodes = nullptr;
      bool edges = false;
      if (connection.is_array())
        nodes = &connection;
      else if (connection.is_object()) {
        if (connection.contains("nodes"))
          nodes = &connection.at("nodes");
        else if (connection.contains("edges")) {
          nodes = &connection.at("edges");
          edges = true;
        }
      }
      if (!nodes || !nodes->is_array()) return {failure(ErrorCode::invalid_response)};
      for (const auto& entry : *nodes) {
        const auto& node = edges ? entry.at("node") : entry;
        const auto& item = node.at("item");
        if (string(item, "id") != app_.app_id) continue;
        std::string platform = string(item, "platform");
        if (platform.empty() && item.contains("latest_supported_binary"))
          platform = string(item["latest_supported_binary"], "platform");
        // This is a Quest app, never a same-title Rift entitlement.
        bool quest_device = false;
        for (auto key : {"supported_hmd_platforms", "supported_hmd_types"}) {
          if (!item.contains(key) || !item[key].is_array()) continue;
          for (const auto& device : item[key]) {
            if (!device.is_string()) continue;
            auto value = device.get<std::string>();
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
              return c >= 'a' && c <= 'z' ? char(c - 'a' + 'A') : char(c);
            });
            for (auto name : {"QUEST", "MONTEREY", "HOLLYWOOD", "SEACLIFF", "EUREKA", "PANTHER"})
              quest_device |= value.find(name) != std::string::npos;
          }
        }
        if (platform != "ANDROID_6DOF" && !(platform == "ANDROID" && quest_device))
          return {failure(ErrorCode::identity_mismatch)};
        if (item.contains("latest_supported_binary") &&
            item["latest_supported_binary"].is_object()) {
          auto package = string(item["latest_supported_binary"], "package_name");
          if (!package.empty() && package != app_.package)
            return {failure(ErrorCode::identity_mismatch)};
        }
        return {};
      }
      // Missing/partial pagination must not assert that the user is unowned.
      // A positive item remains useful even when other pages are unavailable.
      if (connection.is_object() && connection.contains("page_info")) {
        const auto& page = connection["page_info"];
        if (page.contains("has_next_page") && page["has_next_page"].is_boolean() &&
            !page["has_next_page"].get<bool>())
          return {failure(ErrorCode::not_entitled)};
      }
      if (auto answer = item_entitlement(credential, stop)) return std::move(*answer);
      return {failure(ErrorCode::incomplete)};
    } catch (...) {
      return {failure(ErrorCode::invalid_response)};
    }
  }

  Result logged_in_user(std::stop_token stop) override {
    AuthState state;
    if (auto error = authenticate(stop, state)) return {std::move(error)};
    return {{}, state.user, {}};
  }
  Result access_token(std::stop_token stop) override {
    AuthState state;
    if (auto error = authenticate(stop, state)) return {std::move(error)};
    return {{}, state.user, state.token};
  }
  Result user_proof(std::stop_token stop) override {
    AuthState state;
    if (auto error = authenticate(stop, state)) return {std::move(error)};
    try {
      // Signed Meta Platform 1.205 builder RVA 0x158e50. The existing live-proven
      // unversioned form shape is preserved. Nonces are single-use: never retry.
      auto response = http_->post("/user_nonce_generate", {{"access_token", state.token}}, stop);
      log::info("ovrplatform", "Meta /user_nonce_generate http={} fields=[{}]", response.status,
                response_fields(response));
      if (stop.stop_requested()) return {failure(ErrorCode::cancelled)};
      if (store_->load() != state.credential) return {failure(ErrorCode::identity_mismatch)};
      Json body;
      if (auto error = response_error(response, body)) {
        if (error.code == ErrorCode::unauthenticated) invalidate_auth(state.epoch);
        return {std::move(error)};
      }
      {
        std::lock_guard lock(mutex_);
        if (state.epoch != auth_epoch_) return {failure(ErrorCode::unauthenticated)};
      }
      const auto nonce = body.find("nonce");
      if (nonce == body.end() || !nonce->is_string() ||
          nonce->get_ref<const std::string&>().empty() ||
          nonce->get_ref<const std::string&>().find('\0') != std::string::npos) {
        auto error = failure(ErrorCode::invalid_response);
        error.http_status = response.status;
        return {std::move(error)};
      }
      return {{}, {}, nonce->get<std::string>()};
    } catch (...) {
      return {failure(ErrorCode::unavailable)};
    }
  }

  Result heartbeat(std::string_view options, std::stop_token stop) override {
    if (stop.stop_requested()) return {failure(ErrorCode::cancelled)};
    Credential credential;
    { std::lock_guard lock(mutex_); credential = credential_; }
    if (credential.access_token.empty()) return {failure(ErrorCode::uninitialized)};
    try {
      if (store_->load() != credential) return {failure(ErrorCode::identity_mismatch)};
      // Signed Platform 1.205 HeartbeatServiceStandalone (RVA 0x187910).
      // It uses StandalonePlatform's SERVICE token (+0x48 at dispatch), not
      // the app token. Direct CurlHttpClient POST: unversioned URL, query params.
      // No FBNS registration is implemented: push_token remains empty.
      auto response = http_->send({"presence.heartbeat",
          {{"current_status", "ONLINE"}, {"in_vr", "true"}, {"push_token", ""},
           {"rich_presence_options", std::string(options)}, {"app_id_override", app_.app_id}},
          credential.access_token}, stop);
      auto error = http_response_error(response);
      log::info("ovrplatform", "Meta /user_heartbeat http={} error={}", response.status,
                static_cast<int>(error.code));
      if (stop.stop_requested()) return {failure(ErrorCode::cancelled)};
      if (store_->load() != credential) return {failure(ErrorCode::identity_mismatch)};
      if (error) {
        if (error.code == ErrorCode::unauthenticated) invalidate_auth();
        return {std::move(error)};
      }
      auto body = Json::parse(response.body, nullptr, false);
      auto due = body.find("seconds_until_next_call_due");
      if (!body.is_object() || due == body.end() || !due->is_number_integer())
        return {failure(ErrorCode::invalid_response)};
      const auto seconds = due->get<std::int64_t>();
      if (seconds <= 0 || seconds > 86400) return {failure(ErrorCode::invalid_response)};
      return {{}, {}, std::to_string(seconds)};
    } catch (...) { return {failure(ErrorCode::unavailable)}; }
  }
  Result service(std::string_view name, const RequestArgs& args,
                 std::stop_token stop) override {
    // Checkout requires Meta's payment UI. Complete promptly with the
    // standalone error, without authenticating, charging, or inventing a
    // purchase.
    if (name == "ovr_IAP_LaunchCheckoutFlow")
      return {unsupported_operation(name)};
    if (args.error) return {args.error};
    if (name == "ovr_IAP_GetViewerPurchasesDurableCache") {
      Credential credential;
      {
        std::lock_guard lock(mutex_);
        credential = credential_;
      }
      try {
        if (stop.stop_requested()) return {failure(ErrorCode::cancelled)};
        if (credential.access_token.empty())
          return {failure(ErrorCode::uninitialized)};
        if (store_->load() != credential)
          return {failure(ErrorCode::identity_mismatch)};
        return cache().read(credential.account_id);
      } catch (...) {
        return {failure(ErrorCode::unavailable)};
      }
    }
    HttpRequest request;
    try {
      request = identity_iap_request(name, args);
    } catch (...) {
      return {failure(ErrorCode::invalid_argument)};
    }
    AuthState state;
    if (auto error = authenticate(stop, state)) return {std::move(error)};
    request.bearer = state.token;
    HttpResponse response;
    try {
      const bool purchases = name == "ovr_IAP_GetViewerPurchases" ||
                             name == "ovr_IAP_GetNextPurchaseArrayPage";
      const auto cache_generation = purchases ? cache().generation() : 0;
      response = http_->send(request, stop);
      // Never log the numeric path, paging URL, SKU, or server text.
      log::info("ovrplatform", "Meta service={} http={} fields=[{}]", name,
                response.status, response_fields(response));
      if (stop.stop_requested()) return {failure(ErrorCode::cancelled)};
      if (store_->load() != state.credential)
        return {failure(ErrorCode::identity_mismatch)};
      Json body;
      if (auto error = response_error(response, body)) {
        if (error.code == ErrorCode::unauthenticated)
          invalidate_auth(state.epoch);
        return {std::move(error)};
      }
      {
        std::lock_guard lock(mutex_);
        if (state.epoch != auth_epoch_)
          return {failure(ErrorCode::unauthenticated)};
      }
      Model model;
      try {
        model = identity_iap_model(name, body);
        if (name == "ovr_User_Get" &&
            std::get<std::uint64_t>(model.fields.at("ID")) !=
                std::get<std::uint64_t>(args.values.at(0)))
          throw std::invalid_argument("Mismatched Platform user");
      } catch (...) {
        auto error = failure(ErrorCode::invalid_response);
        error.http_status = response.status;
        return {std::move(error)};
      }
      if (purchases)
        cache().page(state.credential.account_id, request.next_url, body,
                     cache_generation);
      else if (name == "ovr_IAP_ConsumePurchase")
        cache().consumed(state.credential.account_id,
                         std::get<std::string>(args.values.at(0)));
      return {{}, {}, {}, std::move(model)};
    } catch (...) {
      return {failure(ErrorCode::unavailable)};
    }
  }

  HttpResponse graph_request(HttpRequest request, std::stop_token stop) override {
    AuthState state;
    if (auto error = authenticate(stop, state)) return {0, {}, std::move(error)};
    try {
      request.bearer = state.token;
      request.app_id = app_.app_id;
      auto response = http_->send(request, stop);
      if (stop.stop_requested()) return {0, {}, failure(ErrorCode::cancelled)};
      if (store_->load() != state.credential)
        return {0, {}, failure(ErrorCode::identity_mismatch)};
      response.error = http_response_error(response);
      if (response.error.code == ErrorCode::unauthenticated) invalidate_auth(state.epoch);
      {
        std::lock_guard lock(mutex_);
        if (state.epoch != auth_epoch_ && !response.error)
          return {0, {}, failure(ErrorCode::unauthenticated)};
      }
      // Never log request parameters, paging URLs or server values.
      log::info("ovrplatform", "Meta endpoint={} http={} fields=[{}]", request.endpoint,
                response.status, response_fields(response));
      return response;
    } catch (...) {
      return {0, {}, failure(ErrorCode::unavailable)};
    }
  }

 private:
  // The store listing of this app as the viewer sees it (the launcher's
  // details query): is_viewer_entitled for the app alone. Empty when Meta gives
  // no clear answer for this app, its package and this account.
  std::optional<Result> item_entitlement(const Credential& credential, std::stop_token stop) {
    try {
      auto response = http_->post(
          "/graphql",
          {{"access_token", credential.access_token},
           {"doc_id", "6549406941839522"},
           {"variables", Json{{"itemId", app_.app_id}, {"hmdType", "EUREKA"}}.dump()}},
          stop);
      if (stop.stop_requested() || entitlement_store_->load() != credential) return {};
      Json body;
      if (response_error(response, body)) return {};
      const auto& data = body.at("data");
      const auto& item = data.at("item");
      auto user_id = string(data.at("viewer").at("user"), "id");
      if (string(item, "id") != app_.app_id || !valid_id(user_id) ||
          (!credential.account_id.empty() && credential.account_id != user_id) ||
          !item.contains("is_viewer_entitled") || !item["is_viewer_entitled"].is_boolean())
        return {};
      if (item.contains("latest_supported_binary") && item["latest_supported_binary"].is_object()) {
        auto package = string(item["latest_supported_binary"], "package_name");
        if (!package.empty() && package != app_.package) return {};
      }
      const bool entitled = item["is_viewer_entitled"].get<bool>();
      log::info("ovrplatform", "Entitlement from the app's store listing: {}", entitled ? "owned" : "not owned");
      return entitled ? Result{} : Result{failure(ErrorCode::not_entitled)};
    } catch (...) {
      return {};
    }
  }
  IapCache& cache() {
    // Lazy construction stays out of authentication/provider locks. call_once
    // also serializes cache path selection across the three service workers.
    std::call_once(cache_once_, [&] {
      cache_ =
          std::make_unique<IapCache>(store_->platform_directory(app_.app_id));
    });
    return *cache_;
  }
  struct AuthState {
    Credential credential;
    User user;
    std::string token;
    std::uint64_t epoch = 0;
  };
  void invalidate_auth(std::uint64_t epoch = 0) {
    std::lock_guard lock(mutex_);
    if (epoch && epoch != auth_epoch_) return;
    ++auth_epoch_;
    app_token_.clear();
    app_user_ = {};
    auth_error_ = failure(ErrorCode::unauthenticated);
    auth_wake_.notify_all();
  }
  Error authenticate(std::stop_token stop, AuthState& state) {
    if (stop.stop_requested()) return failure(ErrorCode::cancelled);
    Credential credential;
    {
      std::lock_guard lock(mutex_);
      credential = credential_;
    }
    if (credential.access_token.empty()) return failure(ErrorCode::uninitialized);
    try {
      if (store_->load() != credential) return failure(ErrorCode::identity_mismatch);
    } catch (...) {
      return failure(ErrorCode::unavailable);
    }
    std::uint64_t epoch;
    {
      std::unique_lock lock(mutex_);
      if (!auth_wake_.wait(lock, stop, [&] { return !authenticating_; }))
        return failure(ErrorCode::cancelled);
      if (stop.stop_requested()) return failure(ErrorCode::cancelled);
      if (auth_error_) return auth_error_;
      if (!app_token_.empty()) {
        state = {credential, app_user_, app_token_, auth_epoch_};
        return {};
      }
      authenticating_ = true;
      epoch = auth_epoch_;
    }
    // Single-flight exchange, but the provider mutex is never held over vault
    // or HTTP I/O. Entitlement and concurrent nonces can proceed independently.
    User user;
    std::string token;
    bool negative_me = false;
    Error error;
    try {
      auto exchange = [&]() -> Error {
        auto response =
            http_->post("/authenticate_application",
                        {{"app_id", app_.app_id}, {"access_token", credential.access_token}}, stop);
        log_auth_response("/authenticate_application", app_.app_id, response);
        if (stop.stop_requested()) return failure(ErrorCode::cancelled);
        Json body;
        if (auto e = response_error(response, body)) return e;
        token = string(body, "access_token");
        if (token.empty()) return failure(ErrorCode::invalid_response);
        if (token == credential.access_token) return failure(ErrorCode::identity_mismatch);
        const auto returned_app = string(body, "app_id");
        if (!returned_app.empty() && returned_app != app_.app_id)
          return failure(ErrorCode::identity_mismatch);
        response = http_->get("/me", {{"access_token", token}, {"fields", "id,alias,display_name"}}, stop);
        log_auth_response("/me", app_.app_id, response);
        if (stop.stop_requested()) return failure(ErrorCode::cancelled);
        if (auto e = response_error(response, body)) {
          negative_me = e.code == ErrorCode::invalid_response ||
                        e.code == ErrorCode::unauthenticated ||
                        (response.status >= 400 && response.status < 500 && response.status != 429);
          return e;
        }
        const auto id = string(body, "id");
        if (!valid_id(id)) {
          negative_me = true;
          auto e = failure(ErrorCode::invalid_response);
          e.http_status = response.status;
          return e;
        }
        std::from_chars(id.data(), id.data() + id.size(), user.id);
        user.alias = string(body, "alias");
        user.display_name = string(body, "display_name");
        if (store_->load() != credential) return failure(ErrorCode::identity_mismatch);
        return {};
      };
      error = exchange();
    } catch (...) {
      error = failure(ErrorCode::unavailable);
    }
    std::lock_guard lock(mutex_);
    authenticating_ = false;
    auth_wake_.notify_all();
    if (epoch != auth_epoch_) return failure(ErrorCode::unauthenticated);
    if (negative_me || error.code == ErrorCode::unauthenticated) auth_error_ = error;
    if (error) return error;
    app_user_ = std::move(user);
    app_token_ = std::move(token);
    state = {credential, app_user_, app_token_, epoch};
    log::info("ovrplatform", "Meta application identity loaded for {}", app_.package);
    return {};
  }
  User app_user_;
  std::string app_token_;
  AppIdentity app_;
  std::shared_ptr<SecretStore> store_;
  std::shared_ptr<SecretStore> entitlement_store_;
  std::shared_ptr<HttpTransport> http_;
  std::mutex mutex_;
  Credential credential_;
  Credential entitlement_credential_;
  std::condition_variable_any auth_wake_;
  bool authenticating_ = false;
  Error auth_error_;
  std::uint64_t auth_epoch_ = 1;
  std::once_flag cache_once_;
  std::unique_ptr<IapCache> cache_;
};
}  // namespace

Error failure(ErrorCode code) {
  const char* text = "Venture: Platform service unavailable";
  switch (code) {
    case ErrorCode::none:
      return {};
    case ErrorCode::unsupported:
      text = "Not supported on this Platform.";
      break;
    case ErrorCode::unauthenticated:
      text = "Venture: sign in to Meta again";
      break;
    case ErrorCode::not_entitled:
      text = "Venture: no active entitlement for this Quest app";
      break;
    case ErrorCode::unavailable:
      text = "Venture: Meta service or network unavailable";
      break;
    case ErrorCode::invalid_response:
      text = "Venture: invalid Meta service response";
      break;
    case ErrorCode::identity_mismatch:
      text = "Venture: app or account session identity changed or mismatched";
      break;
    case ErrorCode::cancelled:
      text = "Venture: Platform request cancelled";
      break;
    case ErrorCode::uninitialized:
      text = "Venture: Platform is not initialized";
      break;
    case ErrorCode::incomplete:
      text = "Venture: incomplete entitlement response; ownership unknown";
      break;
    case ErrorCode::invalid_argument:
      text = "Venture: invalid Platform argument";
      break;
  }
  return {code, 0, text};
}
Error unsupported_operation(std::string_view name) {
  auto error = failure(ErrorCode::unsupported);
  // Signed Meta Platform 1.205 StandalonePlatform panel/dialog slots (plan §2).
  if (name.find("Panel") != name.npos || name.find("Flow") != name.npos ||
      name.find("Dialog") != name.npos || name.starts_with("ovr_Media_") ||
      name.starts_with("ovr_Livestreaming_") || name == "ovr_Application_LaunchOtherApp" ||
      name == "ovr_CloudStorage2_GetUserDirectoryPath")
    error.message = "Not supported on Standalone platform.";
  return error;
}
bool valid_id(std::string_view text) {
  if (text.empty() || text.size() > 20 || text.front() == '0') return false;
  std::uint64_t value = 0;
  auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size() && value != 0;
}
AppIdentity read_app_identity(const std::filesystem::path& plan, std::string_view package,
                              std::int64_t version) {
  std::ifstream file(plan, std::ios::binary);
  if (!file || std::filesystem::file_size(plan) > 1024 * 1024)
    throw std::runtime_error("Invalid Platform launch plan");
  auto j = Json::parse(file, nullptr, false);
  if (!j.is_object() || !j.contains("versionCode") || !j["versionCode"].is_number_integer())
    throw std::runtime_error("Invalid Platform launch plan");
  AppIdentity app{string(j, "appId"), string(j, "package"), string(j, "binaryId"),
                  j.at("versionCode").get<std::int64_t>()};
  app.version_name = string(j, "version");
  // An import that matched no Meta build has no binary id, and its app id only
  // when Meta's was known; platform calls then get what Meta's servers answer
  // for that identity (or report it as unknown).
  const bool local = string(j, "source") == "import" && app.binary_id.empty();
  if ((local ? !app.app_id.empty() && !valid_id(app.app_id)
             : !valid_id(app.app_id) || !valid_id(app.binary_id)) || app.package.empty() ||
      app.package != package || app.version_code <= 0 || app.version_code != version)
    throw std::runtime_error("Platform launch plan does not match the APK");
  return app;
}
std::shared_ptr<Provider> owned_library_provider(
    AppIdentity app, std::shared_ptr<SecretStore> store,
    std::shared_ptr<HttpTransport> http) {
  return owned_library_provider(std::move(app), LaunchCredentials{store, store},
                                std::move(http));
}
std::shared_ptr<Provider> owned_library_provider(
    AppIdentity app, LaunchCredentials stores,
    std::shared_ptr<HttpTransport> http) {
  return std::make_shared<OwnedLibrary>(std::move(app), std::move(stores),
                                        std::move(http));
}
AuthProbe probe_application_auth(const AppIdentity& app, SecretStore& store, HttpTransport& http,
                                 std::stop_token stop) {
  AuthProbe out;
  try {
    if (!valid_id(app.app_id)) {
      out.error = failure(ErrorCode::invalid_argument);
      return out;
    }
    auto credential = store.load();
    if (credential.access_token.empty()) {
      out.error = failure(ErrorCode::unauthenticated);
      return out;
    }
    auto response =
        http.post("/authenticate_application",
                  {{"app_id", app.app_id}, {"access_token", credential.access_token}}, stop);
    out.exchange_status = response.status;
    log_auth_response("/authenticate_application", app.app_id, response);
    Json body;
    if (auto error = response_error(response, body)) {
      out.error = error;
      return out;
    }
    auto candidate = string(body, "access_token");
    out.token_returned = !candidate.empty();
    out.token_differs_from_profile = candidate != credential.access_token;
    if (candidate.empty()) {
      out.error = failure(ErrorCode::invalid_response);
      return out;
    }
    // /me is an investigation only. A returned numeric ID does NOT prove that
    // it is the app-scoped ID that this game's unmodified backend expects.
    response = http.get("/me", {{"access_token", candidate}, {"fields", "id,alias"}}, stop);
    out.identity_status = response.status;
    log_auth_response("/me", app.app_id, response);
    if (auto error = response_error(response, body)) {
      out.error = error;
      return out;
    }
    out.identity_returned = valid_id(string(body, "id"));
    out.identity_matches_profile =
        !credential.account_id.empty() && string(body, "id") == credential.account_id;
    const auto candidate_user = string(body, "id");
    // Probe Graph's token introspection contract on the same Meta endpoint.
    // Availability on graph.oculus.com is not assumed. Only an explicit valid
    // token/app/user tuple could confirm scope; errors never establish it.
    response =
        http.get("/debug_token",
                 {{"input_token", candidate}, {"access_token", credential.access_token}}, stop);
    out.scope_status = response.status;
    log_auth_response("/debug_token", app.app_id, response);
    if (!response_error(response, body) && body.contains("data") && body["data"].is_object()) {
      const auto& data = body["data"];
      out.app_scope_confirmed = data.value("is_valid", false) &&
                                string(data, "app_id") == app.app_id && valid_id(candidate_user) &&
                                string(data, "user_id") == candidate_user;
    }
    if (store.load() != credential) {
      out.error = failure(ErrorCode::identity_mismatch);
      out.app_scope_confirmed = false;
    }
    // Candidate contents are retained only within this diagnostic, never sent
    // to guest code. A nonce-minting path has not been established.
  } catch (...) {
    out.error = failure(ErrorCode::unauthenticated);
  }
  return out;
}
}  // namespace venture::ovrplatform
