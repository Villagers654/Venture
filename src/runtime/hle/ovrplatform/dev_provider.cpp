#include "hle/ovrplatform/dev_provider.h"

#include <nlohmann/json.hpp>

#include "common/log.h"
#include "hle/ovrplatform/identity_iap.h"

namespace venture::ovrplatform {
namespace {
constexpr Id kDevUserId = 1;
constexpr const char* kDevAlias = "Developer";

class DevProvider final : public Provider {
  AppIdentity app_;

 public:
  explicit DevProvider(AppIdentity app) : app_(std::move(app)) {}
  Error prepare() override {
    log::warn("accounts",
              "DEV IDENTITY active for {}: local developer user, no Meta account, "
              "entitlement not checked. Enabled by the per-app dev setting.",
              app_.package);
    return {};
  }
  void declare_app_id(std::string_view id) override { app_.app_id = std::string(id); }
  Result entitlement(std::stop_token stop) override {
    if (stop.stop_requested()) return {failure(ErrorCode::cancelled)};
    return {};
  }
  Result logged_in_user(std::stop_token stop) override {
    if (stop.stop_requested()) return {failure(ErrorCode::cancelled)};
    return {{}, {kDevUserId, kDevAlias, {}}};
  }
  Result access_token(std::stop_token stop) override {
    if (stop.stop_requested()) return {failure(ErrorCode::cancelled)};
    return {{}, {kDevUserId, kDevAlias, {}}, "venture-dev-token"};
  }
  Result user_proof(std::stop_token stop) override {
    if (stop.stop_requested()) return {failure(ErrorCode::cancelled)};
    return {{}, {}, "venture-dev-nonce"};
  }
  Result heartbeat(std::string_view, std::stop_token stop) override {
    if (stop.stop_requested()) return {failure(ErrorCode::cancelled)};
    return {{}, {}, "60"};
  }
  HttpResponse graph_request(HttpRequest request, std::stop_token stop) override {
    if (stop.stop_requested()) return {0, {}, failure(ErrorCode::cancelled)};
    return dev_graph_response(request);
  }
  Result service(std::string_view name, const RequestArgs& args,
                 std::stop_token stop) override {
    if (name == "ovr_IAP_LaunchCheckoutFlow") return {unsupported_operation(name)};
    if (args.error) return {args.error};
    // No local purchase history: the durable cache is simply empty.
    if (name == "ovr_IAP_GetViewerPurchasesDurableCache")
      return {{}, {}, {}, Model{"PurchaseArray", {{"NextUrl", ""}}}};
    try {
      auto request = identity_iap_request(name, args);
      auto response = graph_request(request, stop);
      if (response.error) return {response.error};
      auto body = nlohmann::json::parse(response.body);
      return {{}, {}, {}, identity_iap_model(name, body)};
    } catch (const std::invalid_argument&) {
      return {failure(ErrorCode::invalid_argument)};
    } catch (...) {
      return {failure(ErrorCode::invalid_response)};
    }
  }
};
}  // namespace

HttpResponse dev_graph_response(const HttpRequest& request) {
  const auto& endpoint = request.endpoint;
  if (endpoint == "identity-age")
    return {200, R"({"age_category":"UNKNOWN"})", {}};
  if (endpoint == "identity-user") {
    nlohmann::json body{{"id", request.id},
                        {"alias", kDevAlias},
                        {"presence_status", "UNKNOWN"},
                        {"org_scoped_id", request.id}};
    return {200, body.dump(), {}};
  }
  if (endpoint == "leaderboard-entries")
    return {200, R"({"data":[],"summary":{"total_count":0}})", {}};
  if (endpoint == "leaderboard-write")
    return {200, R"({"did_update":true,"updated_challenge_ids":[]})", {}};
  if (endpoint == "achievement-write") {
    nlohmann::json body{{"api_name", ""}, {"just_unlocked", false}};
    for (const auto& [key, value] : request.parameters)
      if (key == "api_name") {
        body["api_name"] = value;
        break;
      }
    return {200, body.dump(), {}};
  }
  if (endpoint == "social-send-invites") return {200, R"({"invites":[]})", {}};
  if (endpoint == "iap-consume") return {200, R"({})", {}};
  return {200, R"({"data":[]})", {}};
}

bool package_suggests_dev(std::string_view package) {
  size_t start = 0;
  while (start <= package.size()) {
    auto end = package.find('.', start);
    if (end == std::string_view::npos) end = package.size();
    // The last segment is the app's own name, not a namespace.
    if (end != package.size() && package.substr(start, end - start) == "dev") return true;
    start = end + 1;
  }
  return false;
}

std::shared_ptr<Provider> dev_provider(AppIdentity app) {
  return std::make_shared<DevProvider>(std::move(app));
}
}  // namespace venture::ovrplatform
