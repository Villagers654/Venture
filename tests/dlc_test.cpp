#include "common/download/manager.h"
#include "common/library/library.h"
#include "http_loopback.h"
#include <atomic>
#include <doctest/doctest.h>

#include "async_wait.h"

using namespace venture;
namespace {
Json binary() {
  return parse_json(
      R"({"id":"456789","binary_application":{"id":"123456"},"package_name":"com.example.dlc","version":"1.2","version_code":12,"size":4,"asset_files":{"nodes":[
    {"id":"111111","file_name":"required.dat","size":2,"is_required":true},
    {"id":"222222","file_name":"optional.dat","size":3,"is_required":false},
    {"id":"333333","file_name":"ride.bundle","size":5,"is_required":true,"iap_item":{"id":"888888","sku":"ride","public_offer":{"id":"777888"}}},
    {"id":"444444","file_name":"ride-sound.bundle","size":6,"is_required":false,"iap_item":{"id":"888888","sku":"ride"}},
    {"id":"333333","file_name":"collection-ride.bundle","size":5,"is_required":false,"iap_item":{"id":"999999","sku":"collection"}}
  ],"count":5}})");
}
Json purchases(std::string type = "DURABLE", uint64_t expiry = 0) {
  return {{"data",
           Json::array({Json{{"id", "555555"},
                             {"grant_time", 100},
                             {"expiration_time", expiry},
                             {"item", {{"sku", "ride"}, {"type", type}}}}})}};
}

meta::Store::Transport queries(std::atomic<int> *delivery = nullptr) {
  return [delivery](std::string_view url, const http::Fields &fields) {
    auto u = http::parse_url(url);
    if (u.path == "/authenticate_application") {
      if (!u.query.empty() && delivery)
        ++*delivery;
      return Json{{"access_token", "fixture-app-token"}};
    }
    std::map<std::string, std::string> f(fields.begin(), fields.end());
    if (f["doc_id"] == "4850747515044496")
      return parse_json(
          R"({"data":{"viewer":{"user":{"id":"777777","active_entitlements":{"nodes":[{"item":{"id":"123456","platform":"ANDROID_6DOF"}}],"page_info":{"has_next_page":false}}}}}})");
    if (f["doc_id"] == "6549406941839522")
      return Json{{"data",
                   {{"item",
                     {{"id", "123456"},
                      {"platform", "ANDROID_6DOF"},
                      {"latest_supported_binary", binary()}}}}}};
    return Json{{"data", {{"node", binary()}}}};
  };
}
meta::Store::ReadTransport loopback(test::Server &server) {
  return [&server](std::string_view url, const http::Fields &headers) {
    auto u = http::parse_url(url);
    return http::get_loopback(server.port, u.path + '?' + u.query, headers);
  };
}
// The full binary query fails with a 5xx every time, as it does for very large
// catalogs; only the OVRBinaryMetadataQuery APK/OBB details are available.
meta::Store::Transport fallback_queries() {
  auto full = queries();
  return [full](std::string_view url, const http::Fields &fields) -> Json {
    std::map<std::string, std::string> f(fields.begin(), fields.end());
    if (f["doc_id"] == "4734929166632773")
      throw meta::RequestError(502, "Fixture error");
    if (f["doc_id"] == "9973165429396704") {
      auto apk = binary();
      apk.erase("asset_files");
      apk["__typename"] = "AndroidBinary";
      return Json{{"data", {{"metadata", apk}}}};
    }
    if (f["doc_id"] == "6549406941839522")
      return Json{{"data",
                   {{"item",
                     {{"id", "123456"},
                      {"platform", "ANDROID_6DOF"},
                      {"latest_supported_binary",
                       {{"id", "456789"}, {"version", "1.2"}}}}}}}};
    return full(url, fields);
  };
}
struct Directory {
  fs::path root =
      fs::temp_directory_path() /
      ("venture-dlc-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  Directory() { fs::create_directories(root); }
  ~Directory() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
};
void wait_job(download::Manager &manager) {
  const bool idle = venture::test::wait_until([&] { return !manager.busy(); });
  INFO("DLC manager busy=", manager.busy(), " jobs=", manager.snapshot().size());
  REQUIRE(idle);
}
void seed(const fs::path &root) {
  auto plan = meta::build_plan("123456", binary());
  auto dir = paths::game_dir(root, plan.package, plan.version_code);
  paths::atomic_write(dir / "plan.json", plan.json().dump());
  paths::atomic_write(dir / "com.example.dlc.apk", "base");
  paths::atomic_write(dir / "required.dat", "ok");
}
} // namespace

TEST_CASE("DLC ownership is durable, unexpired, complete and fail closed") {
  test::Server server;
  meta::Store store("fixture-profile-token", {}, queries(), loopback(server));
  server.push(purchases("CONSUMABLE").dump());
  CHECK(store.owned_skus("123456").empty());
  server.push(purchases("DURABLE", 1).dump());
  CHECK(store.owned_skus("123456").empty());
  server.push(purchases("DURABLE", UINT64_MAX).dump());
  CHECK(store.owned_skus("123456").contains("ride"));
  server.push(R"({"data":[{}]})");
  CHECK_THROWS(store.owned_skus("123456"));
  auto invalid_expiry = purchases();
  invalid_expiry["data"][0]["expiration_time"] = "18446744073709551616";
  server.push(invalid_expiry.dump());
  CHECK_THROWS(store.owned_skus("123456"));
  server.push(
      R"({"error":{"code":190,"message":"fixture-secret server text"}})", 401);
  try {
    store.owned_skus("123456");
    FAIL("Expected rejected read");
  } catch (const std::exception &e) {
    CHECK(std::string(e.what()).contains("HTTP 401"));
    CHECK_FALSE(std::string(e.what()).contains("fixture-secret"));
  }
}
TEST_CASE("DLC paging is bounded and never forwards credentials to a different "
          "origin or edge") {
  test::Server server;
  meta::Store store("fixture", {}, queries(), loopback(server));
  auto first = Json{{"data", Json::array()},
                    {"paging",
                     {{"next", "https://graph.oculus.com/v1.20/application/"
                               "viewer_purchases?after=fixture"}}}};
  server.push(first.dump());
  server.push(purchases().dump());
  CHECK(store.owned_skus("123456").contains("ride"));
  for (auto next :
       {"http://graph.oculus.com/v1.20/application/viewer_purchases",
        "https://graph.oculus.com.evil.test/v1.20/application/viewer_purchases",
        "https://user@graph.oculus.com/v1.20/application/viewer_purchases",
        "https://graph.oculus.com:444/v1.20/application/viewer_purchases",
        "https://graph.oculus.com/v1.20/me",
        "https://graph.oculus.com/v1.20/application/"
        "viewer_purchases?access_token=secret"}) {
    auto before = server.requests().size();
    first["paging"]["next"] = next;
    server.push(first.dump());
    CHECK_THROWS(store.owned_skus("123456"));
    CHECK(server.requests().size() == before + 1);
  }
}
TEST_CASE("DLC worker installs only owned SKU files and uninstall preserves "
          "base and catalog") {
  Directory directory;
  seed(directory.root);
  test::Server server;
  server.push(purchases().dump());
  std::atomic<int> transfers{0}, delivery{0};
  download::Manager::Services services{
      queries(&delivery), loopback(server),
      [&](std::string, const fs::path &path, uint64_t size, std::stop_token,
          const http::Progress &progress) {
        ++transfers;
        paths::atomic_write(path, std::string(size, 'X'));
        progress(size, size);
      }};
  download::Manager manager(directory.root / "state", [] {}, services);
  manager.set_account("0123456789ab"); // downloads belong to an account
  manager.enqueue_addon("123456", "Test ride", directory.root, "456789", "ride",
                        "fixture");
  wait_job(manager);
  REQUIRE(manager.snapshot()[0].state == download::State::complete);
  CHECK(manager.snapshot()[0].done == 11);
  CHECK(transfers == 2);
  CHECK(delivery == 1);
  auto installs = library::scan(directory.root);
  REQUIRE(installs.size() == 1);
  CHECK(installs[0].complete);
  CHECK(library::addon_installed(installs[0], "ride"));
  CHECK_FALSE(fs::exists(installs[0].directory / "collection-ride.bundle"));
  CHECK_FALSE(fs::exists(installs[0].directory / "optional.dat"));
  auto published = paths::read(installs[0].directory / "plan.json");
  CHECK_FALSE(published.contains("fixture"));
  server.push(Json{{"data", Json::array()}}.dump());
  manager.set_account("0123456789ab"); // downloads belong to an account
  manager.enqueue_addon("123456", "Test collection", directory.root, "456789",
                        "collection", "fixture");
  wait_job(manager);
  REQUIRE(manager.snapshot()[1].state == download::State::failed);
  CHECK(transfers == 2);
  CHECK(delivery == 1);
  manager.uninstall_addon("123456", directory.root, "456789", "ride");
  installs = library::scan(directory.root);
  CHECK(installs[0].complete);
  CHECK_FALSE(library::addon_installed(installs[0], "ride"));
  CHECK_FALSE(fs::exists(installs[0].directory / "ride.bundle"));
  CHECK_FALSE(fs::exists(installs[0].directory / "ride-sound.bundle"));
  CHECK(installs[0].plan.files.size() == 6);
  CHECK(paths::read(installs[0].directory / "com.example.dlc.apk") == "base");
}
TEST_CASE("DLC base download persists full catalog but does not fetch optional "
          "or paid files") {
  Directory directory;
  std::atomic<int> transferred{0};
  download::Manager manager(
      directory.root / "state", [] {},
      {queries(),
       {},
       [&](std::string, const fs::path &path, uint64_t size, std::stop_token,
           const http::Progress &progress) {
         ++transferred;
         paths::atomic_write(path, std::string(size, 'X'));
         progress(size, size);
       }});
  manager.set_account("0123456789ab"); // downloads belong to an account
  manager.enqueue("123456", "Game", directory.root, "fixture");
  wait_job(manager);
  REQUIRE(manager.snapshot()[0].state == download::State::complete);
  CHECK(transferred == 2);
  auto installs = library::scan(directory.root);
  REQUIRE(installs.size() == 1);
  CHECK(installs[0].complete);
  CHECK(installs[0].plan.files.size() == 6);
  // The launcher's smoke install follows exactly this job and plan.
  auto job = download::base_job(manager.snapshot(), "123456");
  REQUIRE(job);
  CHECK(job->state == download::State::complete);
  CHECK(installs[0].plan.launcher_format);
  CHECK(installs[0].plan.assets_complete);
}
TEST_CASE(
    "DLC cancellation saves the exact SKU job and resume rechecks ownership") {
  Directory directory;
  seed(directory.root);
  test::Server server;
  server.push(purchases().dump());
  std::atomic<bool> entered{false}, pause{true};
  download::Manager manager(
      directory.root / "state", [] {},
      {queries(), loopback(server),
       [&](std::string, const fs::path &path, uint64_t size,
           std::stop_token stop, const http::Progress &progress) {
         if (pause) {
           auto part = path;
           part += ".part";
           paths::atomic_write(part, "XX");
           progress(2, size);
           entered = true;
           while (!stop.stop_requested())
             std::this_thread::sleep_for(std::chrono::milliseconds(1));
           throw std::runtime_error("paused");
         }
         paths::atomic_write(path, std::string(size, 'X'));
         auto part = path;
         part += ".part";
         fs::remove(part);
         progress(size, size);
       }});
  manager.set_account("0123456789ab"); // downloads belong to an account
  manager.enqueue_addon("123456", "Test ride", directory.root, "456789", "ride",
                        "fixture");
  REQUIRE(venture::test::wait_until([&] { return entered.load(); }));
  CHECK_THROWS(
      manager.uninstall_addon("123456", directory.root, "456789", "ride"));
  manager.cancel("123456");
  wait_job(manager);
  CHECK(manager.snapshot()[0].state == download::State::paused);
  auto installs = library::scan(directory.root);
  CHECK(installs[0].complete);
  CHECK_FALSE(library::addon_installed(installs[0], "ride"));
  auto queue = paths::read_json(directory.root / "state/downloads.json");
  CHECK(queue["jobs"][0]["sku"] == "ride");
  CHECK(queue["jobs"][0]["binaryId"] == "456789");
  CHECK_FALSE(queue.dump().contains("fixture"));
  {
    download::Manager restored(directory.root / "state", [] {});
    restored.set_account("0123456789ab"); // snapshots are per account
    REQUIRE(restored.snapshot().size() == 1);
    CHECK(restored.snapshot()[0].sku == "ride");
    CHECK(restored.snapshot()[0].binary_id == "456789");
    CHECK(restored.snapshot()[0].state == download::State::paused);
  }
  pause = false;
  server.push(Json{{"data", Json::array()}}.dump());
  manager.resume(manager.snapshot()[0], "fixture");
  wait_job(manager);
  CHECK(manager.snapshot()[0].state == download::State::failed);
  CHECK(fs::exists(installs[0].directory / "ride.bundle.part"));
  CHECK_FALSE(fs::exists(installs[0].directory / "ride.bundle"));
  server.push(purchases().dump());
  manager.resume(manager.snapshot()[0], "fixture");
  wait_job(manager);
  CHECK(manager.snapshot()[0].state == download::State::complete);
  installs = library::scan(directory.root);
  CHECK(library::addon_installed(installs[0], "ride"));
  CHECK(server.requests().size() == 3);
}
TEST_CASE("DLC fallback catalogs never replace an installed catalog or count as "
          "complete") {
  Directory directory;
  seed(directory.root);
  auto dir = paths::game_dir(directory.root, "com.example.dlc", "12");
  auto installed_plan = paths::read(dir / "plan.json");
  auto catalog =
      meta::Store("fixture", {}, fallback_queries()).owned_plan("123456", "456789");
  CHECK_THROWS(library::refresh_catalog(directory.root, catalog));
  CHECK(paths::read(dir / "plan.json") == installed_plan);

  std::atomic<int> transfers{0};
  download::Manager::Services services{
      fallback_queries(), {},
      [&](std::string, const fs::path &path, uint64_t size, std::stop_token,
          const http::Progress &progress) {
        ++transfers;
        paths::atomic_write(path, std::string(size, 'X'));
        progress(size, size);
      }};
  download::Manager manager(directory.root / "state", [] {}, services);
  manager.set_account("0123456789ab");
  manager.enqueue_addon("123456", "Test ride", directory.root, "456789", "ride",
                        "fixture");
  wait_job(manager);
  REQUIRE(manager.snapshot()[0].state == download::State::failed);
  CHECK(manager.snapshot()[0].error.find("temporarily unavailable") !=
        std::string::npos);
  CHECK(transfers == 0);
  // A base resume over a full catalog keeps that catalog.
  manager.enqueue("123456", "Game", directory.root, "fixture");
  wait_job(manager);
  REQUIRE(manager.snapshot()[1].state == download::State::complete);
  CHECK(paths::read(dir / "plan.json") == installed_plan);
  auto installs = library::scan(directory.root);
  REQUIRE(installs.size() == 1);
  CHECK(installs[0].complete);

  // A fresh install from the reduced catalog is playable from its APK and OBB;
  // its plan.json records that the asset catalog is missing.
  Directory fresh;
  manager.enqueue("123456", "Game", fresh.root, "fixture");
  wait_job(manager);
  REQUIRE(manager.snapshot().size() == 2);  // the base job is requeued
  REQUIRE(manager.snapshot()[1].state == download::State::complete);
  auto fresh_dir = paths::game_dir(fresh.root, "com.example.dlc", "12");
  CHECK(paths::read_json(fresh_dir / "plan.json")["assetCatalog"] == "unavailable");
  installs = library::scan(fresh.root);
  REQUIRE(installs.size() == 1);
  CHECK(fs::file_size(installs[0].directory / "com.example.dlc.apk") == 4);
  CHECK(installs[0].complete);
  CHECK_FALSE(installs[0].plan.assets_complete);

  // Once a full query succeeds, refreshing the catalog clears the marker; the
  // now listed required asset makes the install resumable.
  auto full = meta::Store("fixture", {}, queries()).owned_plan("123456", "456789");
  library::refresh_catalog(fresh.root, full);
  CHECK_FALSE(paths::read_json(fresh_dir / "plan.json").contains("assetCatalog"));
  installs = library::scan(fresh.root);
  REQUIRE(installs.size() == 1);
  CHECK(installs[0].plan.assets_complete);
  CHECK_FALSE(installs[0].complete);
  // A base resume with the full catalog also fetches it.
  download::Manager resumed(fresh.root / "state", [] {},
                            {queries(), {}, services.transfer});
  resumed.set_account("0123456789ab");
  resumed.enqueue("123456", "Game", fresh.root, "fixture");
  wait_job(resumed);
  REQUIRE(resumed.snapshot()[0].state == download::State::complete);
  installs = library::scan(fresh.root);
  REQUIRE(installs.size() == 1);
  CHECK(installs[0].complete);
}
// Meta fails the whole-library query (HTTP 500 every time) for accounts with
// very many games; each app's listing still says whether the account owns it.
TEST_CASE("ownership comes from the app's own listing when Meta cannot list the library") {
  for (bool entitled : {true, false}) {
    auto full = queries();
    meta::Store store("fixture", {}, [&](std::string_view url, const http::Fields &fields) -> Json {
      std::map<std::string, std::string> f(fields.begin(), fields.end());
      if (f["doc_id"] == "4850747515044496")
        throw meta::RequestError(500, "Fixture error");
      auto response = full(url, fields);
      if (f["doc_id"] == "6549406941839522") {
        response["data"]["item"]["is_viewer_entitled"] = entitled;
        response["data"]["viewer"] = {{"user", {{"id", "777777"}}}};
      }
      return response;
    });
    auto listing = store.owned_listing("123456");
    CHECK(listing.account_id == "777777");
    CHECK_FALSE(listing.complete);
    if (entitled) {
      REQUIRE(listing.games.size() == 1);
      CHECK(listing.games[0].id == "123456");
      CHECK(listing.games[0].owned);
      CHECK(store.owned_plan("123456").binary_id == "456789");
      CHECK(store.owned_plan("123456", "456789").binary_id == "456789");
    } else {
      CHECK(listing.games.empty());
      CHECK_THROWS(store.owned_plan("123456"));
      CHECK_THROWS(store.owned_plan("123456", "456789"));
    }
  }
  // A listing without an answer is not taken as either.
  meta::Store silent("fixture", {}, queries());
  CHECK_THROWS(silent.owned_listing("123456"));
}
