#include "common/environment.h"
#include "hle/ovrplatform/broker.h"
#include "hle/ovrplatform/identity_iap.h"
#include "http_loopback.h"
#include "common/file_ops.h"
#include "runtime/ovrplatform_support.h"

using namespace venture::ovrplatform;
namespace optest = venture::ovrplatform::test;
namespace {
using venture::test::Server;
using Json = venture::Json;
Json library(bool entitled = true, bool complete = true) {
  auto nodes = Json::array();
  if (entitled)
    nodes.push_back(
        {{"item",
          {{"id", optest::app().app_id}, {"platform", "ANDROID_6DOF"}}}});
  return {{"data",
           {{"viewer",
             {{"user",
               {{"id", "42"},
                {"alias", "root-alias-never-exposed"},
                {"active_entitlements",
                 {{"nodes", nodes},
                  {"page_info", {{"has_next_page", !complete}}}}}}}}}}}};
}
} // namespace

TEST_CASE("credential broker sends only app credentials, refreshes on 190, and "
          "revokes on sign-out") {
  Server server;
  server.push(R"({"access_token":"fake-app-only-token"})");
  server.push(R"({"id":"123456","alias":"fixture-app-alias"})");
  server.push(library().dump());
  auto store = std::make_shared<optest::FakeStore>();
  auto http = loopback_http_transport(server.port);
  register_identity_iap_endpoints(*http);
  auto broker = std::make_shared<CredentialBroker>(optest::app(),
                                                   "123456abcdef", store, http);
  REQUIRE_FALSE(broker->prepare());
  auto packet = broker->handle(venture::Json{{"op", "credentials"}});
  CHECK(packet.dump().find(store->value.access_token) == std::string::npos);
  CHECK_FALSE(packet.contains("accountId"));
  CHECK_FALSE(packet.contains("profileToken"));
  CHECK(venture::str(packet, "appToken") == "fake-app-only-token");
  // Entitlement is asked only when the game asks for it.
  CHECK_FALSE(packet.contains("entitlementError"));
  CHECK(server.requests().size() == 2);
  for (const auto &wire : server.requests()) CHECK(wire.find("/graphql") == wire.npos);
  auto channels = venture::PipeChannel::pair();
  std::jthread worker([&](std::stop_token stop) {
    channels[0]->serve(
        [broker](const venture::Json &r) { return broker->handle(r); }, stop);
  });
  struct Close {
    std::array<std::shared_ptr<venture::PipeChannel>, 2> &channels;
    std::jthread &worker;
    ~Close() {
      channels[1]->close();
      worker.request_stop();
      worker.join();
    }
  } close{channels, worker};
  auto child =
      brokered_provider(optest::app(), "123456abcdef", channels[1], http);
  REQUIRE_FALSE(child->prepare());
  CHECK(server.requests().size() == 2);
  CHECK_FALSE(child->entitlement({}).error);
  CHECK(server.requests().back().find("/graphql") != std::string::npos);
  CHECK(child->logged_in_user({}).user.id == 123456);
  CHECK(child->access_token({}).text == "fake-app-only-token");
  SUBCASE("presence shares the broker channel without exposing its service token") {
    server.push(R"({"seconds_until_next_call_due":60})");
    const auto heartbeat = child->heartbeat(R"({"api_name":"menu"})", {});
    CHECK_FALSE(heartbeat.error);
    CHECK(heartbeat.text == "60");
    const auto wire = server.requests().back();
    CHECK(wire.starts_with("POST /user_heartbeat?"));
    CHECK(wire.find("Authorization: Bearer fake-session-not-a-real-token") != wire.npos);
    CHECK(wire.find("fake-app-only-token") == wire.npos);
    server.push(R"({"seconds_until_next_call_due":60})");
    const auto result = broker->handle({{"op", "presence_heartbeat"}, {"presence", ""}});
    CHECK(result.at("error") == 0);
    CHECK(result.dump().find(store->value.access_token) == std::string::npos);
    store->replace();
    CHECK(child->heartbeat("", {}).error.code == ErrorCode::identity_mismatch);
    return;
  }
  SUBCASE("identity and IAP use the launch account without profile credentials") {
#ifdef _WIN32
    struct CacheRoot {
      std::filesystem::path root = std::filesystem::temp_directory_path() /
          ("venture-broker-iap-" + venture::random_account_key());
      std::wstring previous;
      CacheRoot() {
        if (auto value = _wgetenv(L"VENTURE_DATA_DIR")) previous = value;
        _wputenv_s(L"VENTURE_DATA_DIR", root.c_str());
        venture::environment::refresh();
        venture::paths::atomic_write(root / "accounts/index.json", R"({
          "version":2,"primary":"123456abcdef","active":"123456abcdef",
          "accounts":[{"key":"123456abcdef","accountId":"42","signedIn":true}]
        })");
      }
      ~CacheRoot() {
        _wputenv_s(L"VENTURE_DATA_DIR", previous.c_str());
        venture::environment::refresh();
        venture::remove_tree(root.parent_path(), root);
      }
    } cache_root;
#endif
    server.push(R"({"age_category":"AD"})");
    auto age = child->service("ovr_UserAgeCategory_Get", {}, {});
    REQUIRE_FALSE(age.error);
    CHECK(std::get<std::int32_t>(age.model.fields.at("AgeCategory")) == 3);
    server.push(R"({"id":"123456","alias":"fixture-app-alias"})");
    CHECK_FALSE(child->service("ovr_User_Get", {{std::uint64_t{123456}}}, {}).error);
    server.push(R"({"org_scoped_id":"18446744073709551615"})");
    auto org = child->service("ovr_User_GetOrgScopedID", {{std::uint64_t{123456}}}, {});
    REQUIRE_FALSE(org.error);
    CHECK(std::get<std::uint64_t>(org.model.fields.at("ID")) == UINT64_MAX);
    server.push(R"({"data":[]})");
    CHECK_FALSE(child->service("ovr_IAP_GetProductsBySKU",
                              {{std::vector<std::string>{}, std::int32_t{0}}}, {}).error);
    server.push(R"({"data":[{"id":"10","grant_time":1,"item":{"sku":"sku","type":"DURABLE"}}]})");
    CHECK_FALSE(child->service("ovr_IAP_GetViewerPurchases", {}, {}).error);
#ifdef _WIN32
    auto cached = child->service("ovr_IAP_GetViewerPurchasesDurableCache", {}, {});
    REQUIRE_FALSE(cached.error);
    REQUIRE(cached.model.elements.size() == 1);
    CHECK(std::get<std::string>(cached.model.elements[0].fields.at("SKU")) == "sku");
#endif
    server.push(R"({})");
    CHECK_FALSE(child->service("ovr_IAP_ConsumePurchase", {{std::string("sku")}}, {}).error);
#ifdef _WIN32
    cached = child->service("ovr_IAP_GetViewerPurchasesDurableCache", {}, {});
    REQUIRE_FALSE(cached.error);
    CHECK(cached.model.elements.empty());
#endif
    CHECK(child->service("ovr_IAP_LaunchCheckoutFlow", {}, {}).error.code == ErrorCode::unsupported);
    CHECK(server.requests().size() == 9);
    for (std::size_t i = 3; i < 9; ++i)
      CHECK(server.requests()[i].find("fake-session-not-a-real-token") == std::string::npos);
    store->replace();
    CHECK(child->service("ovr_IAP_GetViewerPurchasesDurableCache", {}, {}).error.code == ErrorCode::identity_mismatch);
    CHECK(child->service("ovr_UserAgeCategory_Get", {}, {}).error.code == ErrorCode::uninitialized);
    CHECK(server.requests().size() == 9);
    return;
  }
  bool consume = false;
  int expired_status = 400;
  std::string expired_body =
      R"({"error":{"code":190,"message":"secret-server-text"}})";
  SUBCASE("Graph error in HTTP 200 errors array") {
    expired_status = 200;
    expired_body =
        R"({"errors":[{"code":190,"message":"secret-server-text"}]})";
  }
  SUBCASE("Graph error in HTTP 400 error object") {}
  SUBCASE("consume refreshes revoked credentials without replaying the write") {
    consume = true;
  }
  server.push(expired_body, expired_status);
  server.push(R"({"access_token":"fake-refreshed-app-token"})");
  server.push(R"({"id":"123456","alias":"fixture-app-alias"})");
  auto failed_nonce = consume
      ? child->service("ovr_IAP_ConsumePurchase", {{std::string("sku")}}, {})
      : child->user_proof({});
  CHECK(failed_nonce.error.code == ErrorCode::unauthenticated);
  CHECK(failed_nonce.error.http_status == expired_status);
  CHECK(failed_nonce.text.empty());
  CHECK(child->access_token({}).text == "fake-refreshed-app-token");
  CHECK(server.requests().size() ==
        6); // one nonce POST, no replay; two refresh reads
  CHECK(broker->handle(venture::Json{{"op", "refresh"}})
            .contains("error")); // throttled, no storm
  store->replace();
  CHECK(child->entitlement({}).error.code == ErrorCode::identity_mismatch);
  CHECK(child->access_token({}).text.empty());
  CHECK(server.requests().size() == 6);
}
TEST_CASE("credential broker refuses malformed or different-account packets") {
  auto channels = venture::PipeChannel::pair();
  std::jthread worker([&](std::stop_token stop) {
    channels[0]->serve(
        [](const venture::Json &) {
          return venture::Json{{"version", 1}, {"account", "abcdef123456"}};
        },
        stop);
  });
  auto child = brokered_provider(optest::app(), "123456abcdef", channels[1],
                                 loopback_http_transport(1));
  CHECK(child->prepare().code == ErrorCode::identity_mismatch);
  channels[1]->close();
  worker.request_stop();
  worker.join();
}
