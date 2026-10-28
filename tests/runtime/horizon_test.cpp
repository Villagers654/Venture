#include "ovrplatform_support.h"
#include "hle/ovrplatform/horizon.h"
#include "hle/ovrplatform/identity_iap.h"

#include "../async_wait.h"

using namespace venture::ovrplatform;
using namespace venture::ovrplatform::test;
namespace {
HorizonMessage wait_message(Horizon& horizon, std::uint64_t route = Horizon::global_session) {
  const auto until = std::chrono::steady_clock::now() + venture::test::hang_timeout;
  while (std::chrono::steady_clock::now() < until) {
    if (auto message = horizon.pop(route)) return *message;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  throw std::runtime_error("Horizon completion timed out: route=" + std::to_string(route) +
                           " queued=" + std::to_string(horizon.count(route)));
}
}
TEST_CASE("Horizon auth JSON and cookies share native Platform services") {
  Arena arena;
  auto provider = std::make_shared<FakeProvider>();
  Session session(app(), provider, arena.memory());
  Horizon horizon(session);
  REQUIRE(horizon.setup(app().app_id) == InitResult::success);
  auto native = session.request(spec("ovr_Entitlement_GetIsViewerEntitled"));
  auto request = horizon.request("users", "get_logged_in_user", 1, "{}", -37);
  auto handle = wait_pop(session);
  CHECK(session.get(handle, "ovr_Message_GetRequestID") == native);
  session.free_message(handle);
  CHECK(session.pop() == 0); // C callers cannot steal Horizon completions either.
  auto message = wait_message(horizon);
  CHECK(message.request_id == request);
  CHECK(message.cookie == -37);
  REQUIRE(message.status_code == 0);
  auto user = venture::parse_json(message.data);
  CHECK(user.at("id").get<std::uint64_t>() == 0xfedcba9876543210);
  CHECK(user.at("alias") == "fixture-alias");
  CHECK(user.at("display_name") == "Fixture User");
  CHECK(horizon.count(1) == 0);
  horizon.request("entitlements", "get_is_viewer_entitled", 1, "{}", 42);
  message = wait_message(horizon);
  CHECK(message.status_code == 0);
  CHECK(message.cookie == 42);
  CHECK(venture::parse_json(message.data) == venture::Json::object());
  CHECK(arena.live == 0); // Horizon never allocates a guest handle graph.
}
TEST_CASE("Horizon cannot initialize with a different app or invent identity") {
  Arena arena;
  auto provider = std::make_shared<FakeProvider>();
  provider->prepare_error = failure(ErrorCode::unauthenticated);
  Session session(app(), provider, arena.memory());
  Horizon horizon(session);
  CHECK(horizon.setup("1") != InitResult::success);
  CHECK(horizon.setup(app().app_id) == InitResult::invalid_credentials);
  horizon.request("users", "get_logged_in_user", 1, "{}", 0);
  auto message = wait_message(horizon);
  CHECK(message.status_code == static_cast<int>(ErrorCode::uninitialized));
  CHECK(message.data == "{}");
}
TEST_CASE("Horizon maps shared identity and IAP models to SDK JSON") {
  Arena arena;
  Session session(app(), std::make_shared<FakeProvider>(), arena.memory());
  Horizon horizon(session);
  REQUIRE(horizon.setup(app().app_id) == InitResult::success);
  session.register_handler("ovr_User_GetOrgScopedID", {
    [](Provider&, const RequestArgs&, std::stop_token) {
      return Result{{}, {}, {}, identity_iap_model("ovr_User_GetOrgScopedID", {{"org_scoped_id", "123456789"}})};
    }, "OrgScopedID"});
  horizon.request("users", "get_org_scoped_id", 1, R"({"user_id":"42"})", 1);
  CHECK(venture::parse_json(wait_message(horizon).data) == venture::Json{{"org_scoped_id", 123456789}});
  session.register_handler("ovr_IAP_GetProductsBySKU", {
    [](Provider&, const RequestArgs& args, std::stop_token) {
      if (std::get<std::vector<std::string>>(args.values.at(0)) != std::vector<std::string>{"fixture-sku"} ||
          std::get<std::int32_t>(args.values.at(1)) != 1) return Result{failure(ErrorCode::invalid_argument)};
      return Result{{}, {}, {}, identity_iap_model("ovr_IAP_GetProductsBySKU", venture::parse_json(R"({"data":[{
        "sku":"fixture-sku","type":"DURABLE","current_offer":{"name":"Fixture product",
        "price":{"currency":"USD","amount_in_hundredths":199,"formatted":"$1.99"}}}]})"))};
    }, "ProductArray"});
  horizon.request("iap", "get_products_by_sku", 1, R"({"skus":["fixture-sku"]})", 2);
  auto message = wait_message(horizon);
  REQUIRE(message.status_code == 0);
  auto product = venture::parse_json(message.data)["data"][0];
  CHECK(product["sku"] == "fixture-sku");
  CHECK(product["price"]["amount_in_hundredths"] == 199);
  CHECK(product["formatted_price"] == "$1.99");
  session.register_handler("ovr_IAP_GetViewerPurchases", {
    [](Provider&, const RequestArgs&, std::stop_token) {
      return Result{{}, {}, {}, identity_iap_model("ovr_IAP_GetViewerPurchases", venture::parse_json(R"({"data":[{
        "id":"opaque-fixture-order","item":{"sku":"fixture-sku","type":"DURABLE"},"grant_time":123}]})"))};
    }, "PurchaseArray"});
  horizon.request("iap", "get_viewer_purchases", 1, "{}", 3);
  message = wait_message(horizon);
  REQUIRE(message.status_code == 0);
  auto purchase = venture::parse_json(message.data)["data"][0];
  CHECK(purchase["id"] == "opaque-fixture-order");
  CHECK(purchase["sku"] == "fixture-sku");
  CHECK_FALSE(purchase.contains("purchase_i_d"));
  CHECK_FALSE(purchase.contains("purchase_id"));
}
TEST_CASE("Horizon keeps user IDs wide and maps friends paging and Unicode") {
  Arena arena;
  auto provider = std::make_shared<FakeProvider>();
  Session session(app(), provider, arena.memory());
  Horizon horizon(session);
  REQUIRE(horizon.setup(app().app_id) == InitResult::success);
  session.register_handler("ovr_User_GetLoggedInUserFriends", {
    [](Provider&, const RequestArgs&, std::stop_token) {
      Model users{"UserArray", {{"NextUrl", std::string("https://graph.oculus.com/v1.20/me/friends?after=fixture")}}};
      users.elements.push_back(user_model(UINT64_MAX, "fixture", "\xc3\xa9\xf0\x9f\x98\x80"));
      return Result{{}, {}, {}, std::move(users)};
    }, "UserArray"});
  horizon.request("users", "get_logged_in_user_friends", 1, "{}", 7);
  auto message = wait_message(horizon);
  REQUIRE(message.status_code == 0);
  CHECK(message.data.find("\\ud83d\\ude00") != std::string::npos);
  auto json = venture::parse_json(message.data);
  CHECK(json["data"][0]["id"].get<std::uint64_t>() == UINT64_MAX);
  CHECK(json["data"][0]["display_name"] == "\xc3\xa9\xf0\x9f\x98\x80");
  CHECK(json["paging"]["next"] == "https://graph.oculus.com/v1.20/me/friends?after=fixture");
  CHECK_FALSE(json.contains("next_url"));
}
