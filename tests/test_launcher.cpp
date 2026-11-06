#include "common/environment.h"
#include "launcher/exit_report.h"
#include "launcher/debug_report.h"
#include "launcher/drives.h"
#include "launcher/updater.h"
#include "common/library/library.h"
#include "common/library/save_import.h"
#include "common/download/manager.h"
#include "launcher/options.h"
#include "http_loopback.h"
#include "common/meta/auth.h"
#include "common/meta/store.h"
#include "common/process.h"
#include "common/secret_store.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <doctest/doctest.h>
#include <format>
#include <fstream>
#include <thread>

#include "async_wait.h"

using namespace venture;
namespace {
Json title(std::string id = "123456") {
  return {{"id", id},
          {"display_name", "Test Quest game"},
          {"platform", "ANDROID_6DOF"}};
}
Json library_response(Json list, bool terminal = true) {
  return {{"data",
           {{"viewer",
             {{"user",
               {{"id", "777777"},
                {"alias", "Fixture user"},
                {"active_entitlements",
                 {{"nodes", list},
                  {"page_info", {{"has_next_page", !terminal}}}}}}}}}}}};
}
Json binary() {
  return parse_json(
      R"({"id":"456789","package_name":"com.example.game","version":"1.2","version_code":12,"platform":"ANDROID_6DOF","size":4,"obb_binary":{"id":"111111","file_name":"main.12.com.example.game.obb","size":3},"asset_files":{"nodes":[{"id":"222222","file_name":"required.dat","size":2,"is_required":true},{"id":"333333","file_name":"optional.dat","size":1,"is_required":false},{"id":"444444","file_name":"paid.dat","size":1,"iap_item":{"id":"999999"}}],"count":3}})");
}
struct Directory {
  fs::path root =
      paths::executable().parent_path() / "test-artifacts" /
      ("launcher-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  Directory() { fs::create_directories(root); }
  ~Directory() {
    auto base = paths::executable().parent_path() / "test-artifacts";
    if (paths::within(base, root)) {
      std::error_code ec;
      fs::remove_all(root, ec);
    }
  }
};
} // namespace
TEST_CASE("native SSO callbacks bind to one challenge and reject duplicate "
          "parameters") {
  auto q = "?token=" + meta::sha256("fixture challenge").substr(0, 16) +
           "&blob=fixture%2Bblob";
  CHECK(meta::validate_callback("oculus://success" + q, "fixture challenge") ==
        "fixture+blob");
  CHECK(meta::validate_callback("oculus-client://success" + q,
                                "fixture challenge") == "fixture+blob");
  CHECK_THROWS(
      meta::validate_callback("https://meta.com/" + q, "fixture challenge"));
  CHECK_THROWS(
      meta::validate_callback("oculus://success" + q, "another challenge"));
  CHECK_THROWS(meta::validate_callback(
      "oculus://success" + q + "&token=duplicate", "fixture challenge"));
  CHECK_THROWS(meta::validate_callback(
      "oculus://success" + q + "&blob=duplicate", "fixture challenge"));
  CHECK_THROWS(meta::validate_callback("oculus://success?token=short&blob=x",
                                       "fixture challenge"));
  CHECK_THROWS(meta::validate_callback(
      "oculus://success?token=" + meta::sha256("x").substr(0, 16) + "&blob=",
      "x"));
}
TEST_CASE("auth navigation accepts only HTTPS Meta domain boundaries") {
  for (auto url : {"https://auth.meta.com/native_sso/confirm",
                   "https://facebook.com/", "https://m.facebook.com/login",
                   "https://www.oculus.com/", "https://instagram.com/"})
    CHECK(meta::auth_navigation_allowed(url));
  for (auto url : {"http://meta.com/", "https://meta.com.evil.test/",
                   "https://notmeta.com/", "https://meta.com@evil.test/",
                   "https://user@meta.com/", "file:///test",
                   "https://meta.com:444/", "javascript:alert(1)"})
    CHECK_FALSE(meta::auth_navigation_allowed(url));
  CHECK(meta::auth_navigation_allowed("https://www.fbsbx.com/captcha/recaptcha/iframe/"));
}
TEST_CASE("auth frames accept any plain HTTPS origin, the page stays on Meta") {
  for (auto url : {"https://www.google.com/recaptcha/api2/anchor",
                   "https://client-api.arkoselabs.com/fc/gc/", "about:blank",
                   "https://facebook.com/"})
    CHECK(meta::auth_frame_allowed(url));
  for (auto url : {"http://example.com/", "https://user@example.com/",
                   "https://example.com:8443/", "file:///test",
                   "javascript:alert(1)", "oculus://success?token=x",
                   "data:text/html,x"})
    CHECK_FALSE(meta::auth_frame_allowed(url));
  CHECK_FALSE(meta::auth_navigation_allowed("https://www.google.com/recaptcha/"));
}
TEST_CASE("entitlements support nodes and edges while retaining completeness") {
  auto j = library_response(Json::array({Json{{"item", title()}}}));
  auto s = meta::parse_library(j);
  REQUIRE(s.games.size() == 1);
  CHECK(s.games[0].owned);
  CHECK(s.complete);
  CHECK(s.account_id == "777777");
  auto &c = j["data"]["viewer"]["user"]["active_entitlements"];
  c.erase("nodes");
  c["edges"] = Json::array({Json{{"node", {{"item", title()}}}}});
  CHECK(meta::parse_library(j).games.size() == 1);
  c.erase("page_info");
  CHECK_FALSE(meta::parse_library(j).complete);
  c["page_info"] = {{"has_next_page", true}};
  CHECK_FALSE(meta::parse_library(j).complete);
  c["page_info"] = {{"has_next_page", "false"}};
  CHECK_THROWS(meta::parse_library(j));
}
TEST_CASE("malformed entitlements reject the whole snapshot") {
  for (auto items :
       {Json::array({Json{{"item", {{"id", "bad"}}}}}),
        Json::array({Json{{"missing", "item"}}}), Json::array({nullptr})})
    CHECK_THROWS(meta::parse_library(library_response(items)));
  CHECK_THROWS(meta::parse_library(Json::object()));
  auto j = library_response(Json::array());
  j["data"]["viewer"]["user"]["active_entitlements"] = {
      {"edges", Json::array({Json{{"node", nullptr}}})}};
  CHECK_THROWS(meta::parse_library(j));
}
TEST_CASE("library reconciliation is conservative and account scoped") {
  auto g = meta::card(title());
  g.owned = true;
  g.ownership_account = "777777";
  g.recent = 123;
  g.image = "https://meta.com/cover";
  std::vector<meta::Game> games{g};
  meta::Snapshot s;
  s.account_id = "777777";
  s.complete = false;
  library::merge(games, s, "777777");
  CHECK(games[0].owned);
  s.complete = true;
  library::merge(games, s, "777777");
  CHECK_FALSE(games[0].owned);
  g.owned = true;
  s.games = {g};
  library::merge(games, s, "777777");
  CHECK(games[0].owned);
  CHECK(games[0].recent == 123);
  CHECK_THROWS(library::merge(games, s, "888888"));
  s.account_id = "888888";
  s.games.clear();
  library::merge(games, s, "888888");
  CHECK(games[0].owned);
  s.account_id.clear();
  library::merge(games, s, "777777");
  CHECK(games[0].owned);
}
TEST_CASE("unsafe or incomplete plans cannot start an installation") {
  auto b = binary();
  b["asset_files"]["count"] = 9;
  CHECK_THROWS(meta::build_plan("123456", b));
  b = binary();
  b["asset_files"]["nodes"][0]["file_name"] = "../escape";
  CHECK_THROWS(meta::build_plan("123456", b));
  b = binary();
  b["package_name"] = "../../escape";
  CHECK_THROWS(meta::build_plan("123456", b));
  b = binary();
  b["binary_application"] = {{"id", "888888"}};
  CHECK_THROWS(meta::build_plan("123456", b));
  b = binary();
  b["size"] = nullptr;
  CHECK_THROWS(meta::build_plan("123456", b));
  b = binary();
  b["uri"] = "https://evil.test/apk";
  CHECK_THROWS(meta::build_plan("123456", b));
  b = binary();
  b["platform"] = "PC";
  CHECK_THROWS(meta::build_plan("123456", b));
}

TEST_CASE("binary metadata fallback posts the official APK and OBB query over loopback") {
  test::Server server;
  auto item = title(); item["latest_supported_binary"] = Json{{"id", "456789"}, {"version", "1.2"}};
  server.push(Json{{"data", {{"item", item}}}}.dump());
  for (int attempt = 0; attempt < 3; ++attempt) server.push("{}", 500);
  auto apk = binary(); apk.erase("asset_files"); apk["__typename"] = "AndroidBinary";
  apk["obb_binary"] = {{"id", "111111"}};
  server.push(Json{{"data", {{"metadata", apk}}}}.dump());
  auto obb = binary()["obb_binary"]; obb["__typename"] = "OBBBinary";
  server.push(Json{{"data", {{"metadata", obb}}}}.dump());
  Json assets{{"count", 1}, {"page_info", {{"has_next_page", false}}},
              {"nodes", Json::array({{{"id", "222222"}, {"file_name", "puzzle.pak"}, {"size", 5}}})}};
  server.push(Json{{"data", {{"node", {{"id", "456789"}, {"asset_files", assets}}}}}}.dump());
  meta::Store store("fixture", {}, [&](std::string_view, const http::Fields& fields) {
    auto reply = http::post_loopback(server.port, "/graphql", fields);
    if (reply.status != 200) throw meta::RequestError(reply.status, "Fixture HTTP error");
    return parse_json(reply.body);
  });
  auto plan = store.details("123456").plan;
  REQUIRE(plan.files.size() == 3);
  CHECK(plan.size() == 7); // optional assets are not part of the base install
  CHECK(plan.version == "1.2");
  CHECK(plan.files[0].kind == "apk"); CHECK(plan.files[1].kind == "obb");
  // The reduced asset list still names every downloadable asset.
  CHECK(plan.files[2].kind == "asset"); CHECK(plan.files[2].name == "puzzle.pak");
  // The reduced catalog is marked, and the marker survives plan.json.
  CHECK(plan.json()["assetCatalog"] == "unavailable");
  CHECK(meta::parse_plan(plan.json()).json()["assetCatalog"] == "unavailable");
  auto requests = server.requests(); REQUIRE(requests.size() == 7);
  for (std::size_t i = 1; i < 7; ++i) {
    auto fields = http::query(requests[i].substr(requests[i].find("\r\n\r\n") + 4));
    std::map<std::string, std::string> values(fields.begin(), fields.end());
    CHECK(values["doc_id"] == (i < 4 ? "4734929166632773" : i < 6 ? "9973165429396704" : "24072064135771905"));
    if (i == 6)
      CHECK(parse_json(values["variables"]) == Json{{"binaryID", "456789"}});
    else if (i >= 4)
      CHECK(parse_json(values["variables"]) == Json{{"binary_id", i == 4 ? "456789" : "111111"}});
  }
}

TEST_CASE("reduced asset list names files through the delivery server and drops unusable names") {
  std::atomic<int> exchanges = 0;
  meta::Store store(
      "fixture", {},
      [&](std::string_view url, const http::Fields& fields) -> Json {
        if (http::parse_url(url).path == "/authenticate_application") {
          ++exchanges;
          return Json{{"access_token", "fixture-delivery"}};
        }
        std::map<std::string, std::string> values(fields.begin(), fields.end());
        if (values["doc_id"] == "6549406941839522") {
          auto item = title(); item["latest_supported_binary"] = Json{{"id", "456789"}, {"version", "1.2"}};
          return Json{{"data", {{"item", item}}}};
        }
        if (values["doc_id"] == "4734929166632773") throw meta::RequestError(500, "Fixture error");
        if (values["doc_id"] == "9973165429396704") {
          auto apk = binary(); apk.erase("asset_files"); apk.erase("obb_binary");
          apk["__typename"] = "AndroidBinary";
          return Json{{"data", {{"metadata", apk}}}};
        }
        REQUIRE(values["doc_id"] == "24072064135771905");
        Json nodes = Json::array({{{"id", "300001"}, {"size", 5}}, {{"id", "300002"}, {"size", 6}},
                                  {{"id", "300003"}, {"file_name", "A.PAK"}, {"size", 7}}});
        return Json{{"data", {{"node", {{"id", "456789"}, {"asset_files",
                    {{"count", 3}, {"page_info", {{"has_next_page", false}}}, {"nodes", nodes}}}}}}}};
      },
      {}, {},
      [](std::string_view url) -> std::string {
        CHECK(url.ends_with("access_token=fixture-delivery"));
        return url.find("id=300001") != url.npos ? "a.pak" : "../escape.pak";
      });
  auto plan = store.details("123456").plan;
  CHECK(exchanges == 1);
  CHECK_FALSE(plan.assets_complete);
  std::vector<std::string> assets;
  for (auto& f : plan.files)
    if (f.kind == "asset") assets.push_back(f.name);
  // The unsafe name and the case-insensitive duplicate are left out.
  CHECK(assets == std::vector<std::string>{"a.pak"});
}

TEST_CASE("a reduced catalog replaces only a reduced catalog with no more assets") {
  auto plan = [](bool complete, int assets) {
    meta::Plan p; p.assets_complete = complete;
    for (int i = 0; i < assets; ++i) { meta::File f; f.kind = "asset"; p.files.push_back(f); }
    return p;
  };
  CHECK(meta::catalog_replaces(plan(true, 0), plan(true, 5)));
  CHECK_FALSE(meta::catalog_replaces(plan(false, 9), plan(true, 1)));
  CHECK(meta::catalog_replaces(plan(false, 2), plan(false, 2)));
  CHECK_FALSE(meta::catalog_replaces(plan(false, 1), plan(false, 2)));
}

TEST_CASE("binary metadata fallback is bounded and never handles authorization or malformed replies") {
  for (auto status : {401, 403, 429, 500}) {
    unsigned calls = 0;
    meta::Store store("fixture", {}, [&](std::string_view, const http::Fields& fields) -> Json {
      ++calls;
      std::map<std::string, std::string> values(fields.begin(), fields.end());
      if (values["doc_id"] == "6549406941839522") {
        auto item = title(); item["latest_supported_binary"] = binary();
        return Json{{"data", {{"item", item}}}};
      }
      throw meta::RequestError(status, "Fixture error");
    });
    CHECK_THROWS(store.details("123456"));
    CHECK(calls == (status == 500 ? 5 : 2));
  }
  auto b = binary(); b["asset_files"]["nodes"][0].erase("is_required");
  auto plan = meta::build_plan("123456", b);
  CHECK_FALSE(plan.files[2].base());
  CHECK(plan.size() == 7);
}

TEST_CASE(
    "delivery exchanges once and never replaces the library access token") {
  int exchanges = 0;
  meta::Store store(
      "fixture-profile", {},
      [&](std::string_view url, const http::Fields &fields) {
        if (http::parse_url(url).path == "/authenticate_application") {
          ++exchanges;
          auto args = http::query(http::parse_url(url).query);
          std::map<std::string, std::string> values(args.begin(), args.end());
          CHECK(values["app_id"] == "1481000308606657");
          CHECK(values["access_token"] == "fixture-profile");
          return Json{{"access_token", "fixture-delivery"}};
        }
        std::map<std::string, std::string> values(fields.begin(), fields.end());
        CHECK(values["access_token"] == "fixture-profile");
        return library_response(Json::array());
      });
  meta::File file{"123456", "test.apk", {}, "apk", 4};
  CHECK(store.download_url(file).ends_with("access_token=fixture-delivery"));
  CHECK(store.download_url(file).ends_with("access_token=fixture-delivery"));
  CHECK(exchanges == 1);
  CHECK(store.library().games.empty());
}

TEST_CASE("sign-in identity comes from the account alone, and a failing library is retried, then explained") {
  meta::Store who("fixture", {}, [&](std::string_view url, const http::Fields& fields) -> Json {
    CHECK(http::parse_url(url).path == "/me");
    std::map<std::string, std::string> values(fields.begin(), fields.end());
    CHECK(values["method"] == "GET");
    CHECK(values["fields"] == "id,alias,display_name");
    return Json{{"id", "123456789"}, {"display_name", "Fixture Name"}};
  });
  auto identity = who.identity();
  CHECK(identity.account_id == "123456789");
  CHECK(identity.alias == "Fixture Name");
  meta::Store bad("fixture", {}, [](std::string_view, const http::Fields&) -> Json { return Json{{"id", "x1"}}; });
  CHECK_THROWS(bad.identity());

  for (auto status : {401, 500}) {
    unsigned calls = 0;
    meta::Store store("fixture", {}, [&](std::string_view, const http::Fields&) -> Json {
      ++calls;
      throw meta::RequestError(status, "Meta graphql failed (HTTP " + std::to_string(status) +
                                           " code 1). Try signing in again.");
    });
    try {
      store.library();
      FAIL("library() should throw");
    } catch (const meta::RequestError& e) {
      CHECK(e.status == status);
      if (status == 500) CHECK(std::string(e.what()).find("Refresh") != std::string::npos);
    }
    // A 500 also tries the headset's library, which fails here too.
    CHECK(calls == (status == 500 ? 4 : 1));
  }
}

TEST_CASE("a library Meta keeps failing is listed a page at a time from the headset's library") {
  auto entitlement = [](std::string id, std::string package, bool restricted = false) {
    return Json{{"node",
                 {{"item",
                   {{"id", id},
                    {"display_name", "Game " + id},
                    {"platform", "ANDROID_6DOF"},
                    {"package_name", package},
                    {"uninstall_restricted", restricted},
                    {"latest_supported_binary", {{"__typename", "AndroidBinary"}, {"id", "9" + id}}}}}}}};
  };
  std::vector<std::string> cursors;
  meta::Store store("fixture", {}, [&](std::string_view url, const http::Fields& fields) -> Json {
    std::map<std::string, std::string> f(fields.begin(), fields.end());
    if (http::parse_url(url).path == "/me") return Json{{"id", "777777"}, {"alias", "fixture"}};
    if (f["doc_id"] == "4850747515044496") throw meta::RequestError(500, "Meta graphql failed (HTTP 500 code 1).");
    CHECK(f["client_doc_id"] == "412097616712440770934752951129");
    auto vars = parse_json(f["variables"]);
    auto cursor = vars["cursorID"].is_null() ? std::string() : vars["cursorID"].get<std::string>();
    cursors.push_back(cursor);
    Json edges = cursor.empty() ? Json::array({entitlement("123456", "com.example.one"),
                                               entitlement("234567", "com.oculus.shell", true)})
                                : Json::array({entitlement("345678", "com.example.two"), entitlement("123456", "com.example.one")});
    return Json{{"data",
                 {{"entitlements",
                   {{"active_android_entitlements",
                     {{"edges", edges},
                      {"page_info", {{"has_next_page", cursor.empty()}, {"end_cursor", cursor.empty() ? "page2" : "page3"}}}}}}}}}};
  });
  auto snapshot = store.library();
  CHECK(cursors == std::vector<std::string>{"", "page2"});
  CHECK(snapshot.complete);
  CHECK(snapshot.account_id == "777777");
  REQUIRE(snapshot.games.size() == 3);
  CHECK(snapshot.games[0].id == "123456");
  CHECK(snapshot.games[0].package == "com.example.one");
  CHECK(snapshot.games[0].owned);
  CHECK(snapshot.games[1].system);
  CHECK(snapshot.games[2].id == "345678");
}

TEST_CASE("plans reject collisions with manifests and partial files") {
  for (auto name :
       {"plan.json", "PLAN.JSON", "plan.json.tmp", "payload.part"}) {
    auto b = binary();
    b["asset_files"]["nodes"][0]["file_name"] = name;
    CHECK_THROWS(meta::build_plan("123456", b));
  }
  auto b = binary();
  auto duplicate = b["asset_files"]["nodes"][0];
  duplicate["id"] = "989898";
  b["asset_files"]["nodes"].push_back(duplicate);
  CHECK_THROWS(meta::build_plan("123456", b));
}
TEST_CASE("delivery token is restricted to exact securecdn binary endpoints") {
  auto url =
      http::with_delivery_token("https://securecdn-us.oculus.com/binaries/"
                                "download/?id=123456&access_token=old",
                                "fixture");
  CHECK(url == "https://securecdn-us.oculus.com/binaries/download/"
               "?id=123456&access_token=fixture");
  CHECK(http::with_delivery_token(
            "https://scontent.fbcdn.net/image?signature=sample", "fixture") ==
        "https://scontent.fbcdn.net/image?signature=sample");
  CHECK_FALSE(http::secure_binary(
      "https://securecdn.oculus.com.evil.test/binaries/download/"));
  CHECK_FALSE(http::secure_binary("https://securecdn.oculus.com/other/"));
  CHECK_THROWS(http::with_delivery_token(
      "http://securecdn.oculus.com/binaries/download/", "fixture"));
  CHECK_FALSE(http::allowed_delivery("https://user@meta.com/file"));
}
TEST_CASE("resume checks restart on 200 and validate range and total on 206") {
  auto fresh = http::check_range(200, 40, 100, "", 100);
  CHECK(fresh.offset == 0);
  CHECK(fresh.total == 100);
  auto resume = http::check_range(206, 40, 100, "bytes 40-99/100", 60);
  CHECK(resume.offset == 40);
  CHECK(resume.total == 100);
  CHECK_THROWS(http::check_range(206, 40, 100, "bytes 0-99/100", 100));
  CHECK_THROWS(http::check_range(206, 40, 100, "bytes 40-99/101", 60));
  CHECK_THROWS(http::check_range(206, 40, 100, "bytes 40-99/100", 59));
  CHECK_THROWS(http::check_range(206, 40, 100, "bytes */100", 0));
  CHECK_THROWS(http::check_range(416, 40, 100, "bytes */100", 0));
  CHECK_THROWS(http::check_range(403, 0, 100, "", 0));
}
TEST_CASE("cross platform filenames and path containment") {
  for (auto name : {"../escape", "a/b", "a\\b", "C:drive", "CON", "aux.txt",
                    "LPT1.txt", "trailing.", "trailing ", ""})
    CHECK_FALSE(paths::safe_name(name));
  CHECK(paths::safe_name("main.12.com.example.obb"));
  CHECK(paths::safe_package("com.example.game"));
  // Guest names with characters NTFS forbids round-trip through their host form.
  for (const char *name : {"prefs:id.xml", "a*b?c\"d<e>f|g", "tab\tname", "plain.txt"}) {
    CHECK(paths::guest_name(paths::host_name(name)) == name);
#ifdef _WIN32
    CHECK(paths::host_name(name).find_first_of(":*?\"<>|\t") == std::string::npos);
#else
    CHECK(paths::host_name(name) == name);
#endif
  }
  CHECK(paths::guest_name("plain\xEF\x80\xBA") == "plain" + std::string(paths::host_name(":") == ":" ? "\xEF\x80\xBA" : ":"));
  Directory d;
  CHECK(paths::within(d.root, d.root / "games"));
  CHECK_FALSE(paths::within(d.root, d.root));
  CHECK_FALSE(paths::within(d.root, d.root / ".." / "escape"));
  CHECK_THROWS(paths::game_dir(d.root, "com.example.game", "../1"));
  CHECK_THROWS(paths::game_dir(d.root, "../game", "1"));
  CHECK(paths::from_utf8(
            paths::utf8(d.root / paths::from_utf8("Unicode-\xc3\xa9"))) ==
        d.root / paths::from_utf8("Unicode-\xc3\xa9"));
}
TEST_CASE(
    "scanner recognizes Python plan format and rejects partial installs") {
  Directory d;
  auto p = meta::build_plan("123456", binary());
  auto dir = paths::game_dir(d.root, p.package, p.version_code);
  auto j = p.json();
  j["versionCode"] = 12;
  paths::atomic_write(dir / "plan.json", j.dump());
  paths::atomic_write(dir / p.files[0].name, "ABCD");
  auto result = library::scan(d.root);
  REQUIRE(result.size() == 1);
  CHECK_FALSE(result[0].complete);
  CHECK(result[0].bytes == 4);
  // An interrupted download's partial file counts toward its progress.
  paths::atomic_write(dir / (p.files[1].name + ".part"), "AB");
  CHECK(library::scan(d.root)[0].bytes == 6);
  fs::remove(dir / (p.files[1].name + ".part"));
  paths::atomic_write(dir / p.files[1].name, "ABC");
  paths::atomic_write(dir / p.files[2].name, "AB");
  result = library::scan(d.root);
  CHECK(result[0].complete);
  CHECK(result[0].bytes == 9);
  paths::atomic_write(dir / p.files[2].name, "X");
  CHECK_FALSE(library::scan(d.root)[0].complete);
}
TEST_CASE("only a runtime override is saved; the bundled runtime is never "
          "pinned") {
  Directory d;
  auto settings = library::Settings::defaults(d.root);
  CHECK_FALSE(settings.runtime_override());
  settings.save(d.root);
  CHECK_FALSE(paths::read_json(d.root / "settings.json")
                  .contains("runtimeExecutable"));
  CHECK(library::Settings::load(d.root).runtime == settings.runtime);

  auto custom = d.root / "custom-runtime.exe";
  paths::atomic_write(custom, "fixture");
  settings.runtime = custom;
  CHECK(settings.runtime_override());
  settings.save(d.root);
  CHECK(paths::from_utf8(str(paths::read_json(d.root / "settings.json"),
                             "runtimeExecutable")) == custom);
  auto loaded = library::Settings::load(d.root);
  CHECK(loaded.runtime_override());
  CHECK(loaded.runtime == custom.lexically_normal());

  // An override that was removed falls back to the bundled runtime and is
  // dropped on the next save.
  fs::remove(custom);
  loaded = library::Settings::load(d.root);
  CHECK_FALSE(loaded.runtime_override());
  loaded.save(d.root);
  CHECK_FALSE(paths::read_json(d.root / "settings.json")
                  .contains("runtimeExecutable"));
}
TEST_CASE("library folders are saved, and the install dialog's ask setting "
          "reads the older Steam-only one") {
  Directory d;
  auto settings = library::Settings::defaults(d.root);
  CHECK(settings.roots() == std::vector<fs::path>{settings.games});
  auto other = (d.root / "other").lexically_normal();
  settings.libraries = {other, settings.games};
  settings.install_ask = false;
  settings.game_language = "zh-Hans-CN";
  settings.interface_language = "pt_BR";
  settings.save(d.root);
  auto loaded = library::Settings::load(d.root);
  CHECK(loaded.game_language == "zh-Hans-CN");
  CHECK(loaded.interface_language == "pt_BR");
  CHECK(loaded.libraries == std::vector<fs::path>{other});
  CHECK(loaded.roots() == std::vector<fs::path>{settings.games, other});
  CHECK_FALSE(loaded.install_ask);

  auto json = paths::read_json(d.root / "settings.json");
  json.erase("installAsk");
  json["steamAsk"] = false;
  paths::atomic_write(d.root / "settings.json", json.dump());
  CHECK_FALSE(library::Settings::load(d.root).install_ask);
  json.erase("steamAsk");
  paths::atomic_write(d.root / "settings.json", json.dump());
  CHECK(library::Settings::load(d.root).install_ask);
}
TEST_CASE("display settings save globally and per game, migrate unknown values and "
          "give the runtime its environment") {
  Directory d;
  auto settings = library::Settings::defaults(d.root);
  CHECK(settings.display == library::global_display_defaults());
  CHECK(library::resolution_environment(settings.display) == "runtime");
  CHECK(library::frame_rate_environment(settings.display) == "runtime");
  settings.display.resolution = "percent";
  settings.display.percent = 150;
  settings.display.rate = "120";
  auto game = library::game_display_defaults();
  game.resolution = "custom";
  game.width = 2064;
  game.height = 2208;
  settings.game_display["1"] = game;
  settings.game_display["2"] = library::game_display_defaults();  // all "Use global setting"
  settings.save(d.root);
  auto json = paths::read_json(d.root / "settings.json");
  CHECK(json["gameDisplay"].size() == 1);
  auto loaded = library::Settings::load(d.root);
  CHECK(loaded.display == settings.display);
  REQUIRE(loaded.game_display.contains("1"));
  CHECK(loaded.game_display["1"] == game);
  CHECK_FALSE(loaded.game_display.contains("2"));

  // A game's choice replaces the global one per setting; "Use global setting" keeps it.
  auto effective = library::effective_display(loaded.display, &loaded.game_display["1"]);
  CHECK(library::resolution_environment(effective) == "2064x2208");
  CHECK(library::frame_rate_environment(effective) == "120");
  CHECK(library::resolution_environment(library::effective_display(loaded.display, nullptr)) == "150%");
  game = library::game_display_defaults();
  game.resolution = "quest3";
  game.rate = "custom";
  game.custom_rate = 100;
  effective = library::effective_display(loaded.display, &game);
  CHECK(library::resolution_environment(effective) == "1680x1760");
  CHECK(library::frame_rate_environment(effective) == "100");

  // Values from older builds or edited by hand fall back to the default:
  // the mockup's "Quest 3" frame rate, unknown presets, a custom size without one.
  json["display"] = {{"resolution", "quest9"}, {"frameRate", "quest3"}, {"percent", 1234}};
  json["gameDisplay"] = {{"1", {{"resolution", "custom"}, {"frameRate", "quest3"}}},
                         {"2", {{"resolution", "go"}, {"frameRate", "72"}}}};
  paths::atomic_write(d.root / "settings.json", json.dump());
  loaded = library::Settings::load(d.root);
  CHECK(loaded.display == library::global_display_defaults());
  CHECK_FALSE(loaded.game_display.contains("1"));
  REQUIRE(loaded.game_display.contains("2"));
  CHECK(library::resolution_environment(loaded.game_display["2"]) == "1024x1024");
  CHECK(library::frame_rate_environment(loaded.game_display["2"]) == "72");
}
TEST_CASE("the desktop mirror is on by default, opt out globally or per game") {
  Directory d;
  auto settings = library::Settings::defaults(d.root);
  CHECK(settings.display.mirror == "cropped");
  CHECK(library::mirror_environment(settings.display) == "cropped");
  settings.display.mirror = "full";
  auto off = library::game_display_defaults();
  off.mirror = "off";
  settings.game_display["1"] = off;
  settings.game_display["2"] = library::game_display_defaults();
  settings.save(d.root);
  auto loaded = library::Settings::load(d.root);
  CHECK(library::mirror_environment(loaded.display) == "full");
  CHECK(library::mirror_environment(library::effective_display(loaded.display, &loaded.game_display["1"])) == "off");
  CHECK_FALSE(loaded.game_display.contains("2"));
  CHECK(library::mirror_environment(library::effective_display(loaded.display, nullptr)) == "full");
  // Settings from before the mirror, and unknown values, keep the default.
  auto json = paths::read_json(d.root / "settings.json");
  json["display"] = {{"resolution", "runtime"}, {"mirror", "both"}};
  json["gameDisplay"] = {{"1", {{"resolution", "go"}}}};
  paths::atomic_write(d.root / "settings.json", json.dump());
  loaded = library::Settings::load(d.root);
  CHECK(loaded.display.mirror == "cropped");
  CHECK(loaded.game_display["1"].mirror.empty());
}
TEST_CASE("the desktop mirror shows the left eye by default, the right globally or per game") {
  Directory d;
  auto settings = library::Settings::defaults(d.root);
  CHECK(settings.display.mirror_eye == "left");
  CHECK(library::mirror_eye_environment(settings.display) == "left");
  CHECK(library::game_display_defaults().mirror_eye.empty());
  settings.display.mirror_eye = "right";
  auto left = library::game_display_defaults();
  left.mirror_eye = "left";
  settings.game_display["1"] = left;
  settings.game_display["2"] = library::game_display_defaults();
  settings.save(d.root);
  auto json = paths::read_json(d.root / "settings.json");
  CHECK(json["display"]["mirrorEye"] == "right");
  auto loaded = library::Settings::load(d.root);
  CHECK(loaded.display.mirror_eye == "right");
  CHECK(library::mirror_eye_environment(library::effective_display(loaded.display, nullptr)) == "right");
  CHECK(library::mirror_eye_environment(library::effective_display(loaded.display, &loaded.game_display["1"])) == "left");
  CHECK_FALSE(loaded.game_display.contains("2"));
  // A game following the global eye but with its own mirror mode.
  auto full = library::game_display_defaults();
  full.mirror = "full";
  CHECK(library::mirror_eye_environment(library::effective_display(loaded.display, &full)) == "right");
  // Settings from before the eye choice, and unknown values, keep the default.
  json["display"] = {{"mirror", "full"}, {"mirrorEye", "both"}};
  json["gameDisplay"] = {{"1", {{"mirror", "off"}}}};
  paths::atomic_write(d.root / "settings.json", json.dump());
  loaded = library::Settings::load(d.root);
  CHECK(loaded.display.mirror == "full");
  CHECK(loaded.display.mirror_eye == "left");
  REQUIRE(loaded.game_display.contains("1"));
  CHECK(loaded.game_display["1"].mirror_eye.empty());
}
TEST_CASE("the game's audio on the PC is off by default, on globally or per game") {
  Directory d;
  auto settings = library::Settings::defaults(d.root);
  CHECK(settings.display.pc_audio == "off");
  CHECK(library::pc_audio_environment(settings.display) == "off");
  CHECK(library::game_display_defaults().pc_audio.empty());
  settings.display.pc_audio = "on";
  auto off = library::game_display_defaults();
  off.pc_audio = "off";
  settings.game_display["1"] = off;
  settings.game_display["2"] = library::game_display_defaults();
  settings.save(d.root);
  auto json = paths::read_json(d.root / "settings.json");
  CHECK(json["display"]["pcAudio"] == "on");
  CHECK(json["gameDisplay"]["1"]["pcAudio"] == "off");
  auto loaded = library::Settings::load(d.root);
  CHECK(loaded.display.pc_audio == "on");
  CHECK(library::pc_audio_environment(library::effective_display(loaded.display, nullptr)) == "on");
  CHECK(library::pc_audio_environment(library::effective_display(loaded.display, &loaded.game_display["1"])) == "off");
  CHECK_FALSE(loaded.game_display.contains("2"));
  // A game following the global choice but with its own mirror.
  auto full = library::game_display_defaults();
  full.mirror = "full";
  CHECK(library::pc_audio_environment(library::effective_display(loaded.display, &full)) == "on");
  // Globally off, one game on.
  loaded.display.pc_audio = "off";
  auto on = library::game_display_defaults();
  on.pc_audio = "on";
  CHECK(library::pc_audio_environment(library::effective_display(loaded.display, &on)) == "on");
  // Settings from before the choice, and unknown values, load as off.
  json["display"] = {{"mirror", "full"}, {"mirrorEye", "right"}};
  json["gameDisplay"] = {{"1", {{"mirror", "off"}}}, {"3", {{"pcAudio", "loud"}, {"mirror", "full"}}}};
  paths::atomic_write(d.root / "settings.json", json.dump());
  loaded = library::Settings::load(d.root);
  CHECK(loaded.display.pc_audio == "off");
  CHECK(loaded.display.mirror_eye == "right");
  REQUIRE(loaded.game_display.contains("1"));
  CHECK(loaded.game_display["1"].pc_audio.empty());
  REQUIRE(loaded.game_display.contains("3"));
  CHECK(loaded.game_display["3"].pc_audio.empty());
  CHECK(library::pc_audio_environment(library::effective_display(loaded.display, &loaded.game_display["3"])) == "off");
  json["display"] = {{"pcAudio", true}};
  paths::atomic_write(d.root / "settings.json", json.dump());
  CHECK(library::Settings::load(d.root).display.pc_audio == "off");
}
TEST_CASE("a game's texture compression follows the global one unless it picks its own") {
  Directory d;
  auto settings = library::Settings::defaults(d.root);
  using Choice = std::pair<std::string, int>;
  auto launched = [](const library::Settings& s, const std::string& id) {
    const auto own = s.game_display.find(id);
    return library::texture_compression_environment(s.texture_compression, s.texture_compression_threshold,
                                                    own == s.game_display.end() ? nullptr : &own->second);
  };
  CHECK(settings.texture_compression == 0);
  CHECK(launched(settings, "1") == Choice{"always", 0});
  CHECK(library::game_display_defaults().texture_compression.empty());
  CHECK(library::game_display_defaults().texture_compression_threshold == 0);
  settings.texture_compression = 2;
  auto own_auto = library::game_display_defaults();
  own_auto.texture_compression = "auto";
  own_auto.texture_compression_threshold = 75;
  auto global_threshold = library::game_display_defaults();
  global_threshold.texture_compression = "auto";
  auto always = library::game_display_defaults();
  always.texture_compression = "always";
  settings.game_display["1"] = own_auto;
  settings.game_display["2"] = global_threshold;
  settings.game_display["3"] = always;
  settings.game_display["4"] = library::game_display_defaults();
  settings.save(d.root);
  auto json = paths::read_json(d.root / "settings.json");
  CHECK(json["textureCompression"] == 2);
  CHECK_FALSE(json["display"].contains("textureCompression"));
  CHECK(json["gameDisplay"]["1"]["textureCompression"] == "auto");
  CHECK(json["gameDisplay"]["1"]["textureCompressionThreshold"] == 75);
  CHECK_FALSE(json["gameDisplay"]["2"].contains("textureCompressionThreshold"));
  auto loaded = library::Settings::load(d.root);
  CHECK(loaded.game_display["1"] == own_auto);
  CHECK(loaded.game_display["2"] == global_threshold);
  CHECK(loaded.game_display["3"] == always);
  CHECK_FALSE(loaded.game_display.contains("4"));
  CHECK(launched(loaded, "1") == Choice{"auto", 75});
  CHECK(launched(loaded, "2") == Choice{"auto", 50});
  CHECK(launched(loaded, "3") == Choice{"always", 0});
  CHECK(launched(loaded, "4") == Choice{"never", 0});
  // A global Automatic gives its threshold to games following it.
  loaded.texture_compression = 1;
  loaded.texture_compression_threshold = 25;
  CHECK(launched(loaded, "4") == Choice{"auto", 25});
  CHECK(launched(loaded, "2") == Choice{"auto", 25});
  // A game's threshold counts only with its own Automatic.
  auto never = library::game_display_defaults();
  never.texture_compression = "never";
  never.texture_compression_threshold = 90;
  loaded.game_display["5"] = never;
  CHECK(launched(loaded, "5") == Choice{"never", 0});
  // Settings from before the choice, and unknown values, follow the global setting.
  json = paths::read_json(d.root / "settings.json");
  json["display"] = {{"mirror", "full"}, {"textureCompression", "never"}};
  json["gameDisplay"] = {{"1", {{"mirror", "off"}}},
                         {"3", {{"textureCompression", "sometimes"}, {"textureCompressionThreshold", 99}, {"mirror", "full"}}},
                         {"6", {{"textureCompression", 2}, {"textureCompressionThreshold", 5}}}};
  paths::atomic_write(d.root / "settings.json", json.dump());
  loaded = library::Settings::load(d.root);
  CHECK(loaded.texture_compression == 2);
  CHECK(loaded.display.texture_compression.empty());
  CHECK(loaded.display.mirror == "full");
  REQUIRE(loaded.game_display.contains("1"));
  CHECK(loaded.game_display["1"].texture_compression.empty());
  REQUIRE(loaded.game_display.contains("3"));
  CHECK(loaded.game_display["3"].texture_compression.empty());
  CHECK(loaded.game_display["3"].texture_compression_threshold == 0);
  CHECK_FALSE(loaded.game_display.contains("6"));
  CHECK(launched(loaded, "3") == Choice{"never", 0});
}
TEST_CASE("turning texture compression down from Always warns until the warning is acknowledged") {
  Directory d;
  auto settings = library::Settings::defaults(d.root);
  CHECK_FALSE(settings.texture_compression_warned);
  CHECK(settings.texture_compression_warns("always", "auto"));
  CHECK(settings.texture_compression_warns("always", "never"));
  CHECK_FALSE(settings.texture_compression_warns("always", "always"));
  CHECK_FALSE(settings.texture_compression_warns("auto", "never"));
  CHECK_FALSE(settings.texture_compression_warns("never", "always"));
  // Not saved until acknowledged.
  settings.save(d.root);
  CHECK(paths::read_json(d.root / "settings.json")["textureCompressionWarned"] == false);
  // "Change anyway" sets it; it survives a reload and covers every change.
  settings.texture_compression_warned = true;
  settings.save(d.root);
  auto json = paths::read_json(d.root / "settings.json");
  CHECK(json["textureCompressionWarned"] == true);
  auto loaded = library::Settings::load(d.root);
  CHECK(loaded.texture_compression_warned);
  CHECK_FALSE(loaded.texture_compression_warns("always", "auto"));
  CHECK_FALSE(loaded.texture_compression_warns("always", "never"));
  // Settings from before the warning, and other values, load as not acknowledged.
  json.erase("textureCompressionWarned");
  paths::atomic_write(d.root / "settings.json", json.dump());
  loaded = library::Settings::load(d.root);
  CHECK_FALSE(loaded.texture_compression_warned);
  CHECK(loaded.texture_compression_warns("always", "never"));
  json["textureCompressionWarned"] = "yes";
  paths::atomic_write(d.root / "settings.json", json.dump());
  CHECK_FALSE(library::Settings::load(d.root).texture_compression_warned);
}
TEST_CASE("the runtime's recorded host display gives the percent size, clamped to its maximum") {
  Directory d;
  CHECK_FALSE(library::load_host_display(d.root));
  paths::atomic_write(d.root / "host-display.json",
                      R"({"runtime":"SteamVR/OpenXR","system":"Quest 3","recommended":[2016,2112],)"
                      R"("max":[4032,4224],"rate":90,"rates":[72,80,90,120]})");
  const auto host = library::load_host_display(d.root);
  REQUIRE(host);
  CHECK(host->runtime == "SteamVR/OpenXR");
  CHECK(host->width == 2016);
  CHECK(host->max_height == 4224);
  CHECK(host->rate == doctest::Approx(90));
  CHECK(host->rates == std::vector<double>{72, 80, 90, 120});
  // Each side by the square root of the pixel-count percent.
  CHECK(library::percent_size(*host, 100) == std::pair{2016, 2112});
  CHECK(library::percent_size(*host, 50) == std::pair{1426, 1493});
  CHECK(library::percent_size(*host, 400) == std::pair{4032, 4224});
  auto small = *host;
  small.max_width = 2400;
  small.max_height = 0;  // unknown: only the width limits
  CHECK(library::percent_size(small, 200) == std::pair{2400, 2514});
  CHECK(library::fit_eye_size(8, 8, 0, 0) == std::pair{library::min_custom_side, library::min_custom_side});
  paths::atomic_write(d.root / "host-display.json", R"({"recommended":[0,10]})");
  CHECK_FALSE(library::load_host_display(d.root));
  paths::atomic_write(d.root / "host-display.json", "not json");
  CHECK_FALSE(library::load_host_display(d.root));
}
TEST_CASE("install locations list library folders first, then a new folder on "
          "each other drive") {
#ifdef _WIN32
  const fs::path c = "C:\\", d = "D:\\", e = "E:\\";
  const fs::path games = "c:\\Users\\me\\AppData\\Local\\Venture\\games";
  const fs::path library = "D:\\Games";
#else
  const fs::path c = "/home", d = "/run/media/me/Data", e = "/run/media/me/Data2";
  const fs::path games = "/home/me/.local/share/venture/games";
  const fs::path library = "/run/media/me/Data/Games";
#endif
  library::Settings settings;
  settings.games = games;
  settings.libraries = {library};
  auto locations = launcher::install_locations(settings, {{c, "C"}, {d, "D"}, {e, "E"}});
  REQUIRE(locations.size() == 3);
  CHECK(locations[0].folder == games);
  CHECK(locations[0].drive == "C");
  CHECK(locations[0].library);
  CHECK(locations[1].folder == library);
  CHECK(locations[1].drive == "D");
  // Data2 is its own drive, not inside Data.
  CHECK(locations[2].folder == e / "Venture Games");
  CHECK(locations[2].drive == "E");
  CHECK_FALSE(locations[2].library);
}
TEST_CASE("Windows process command line preserves spaces quotes and trailing "
          "slashes") {
  CHECK(quote_windows_arg(L"") == L"\"\"");
  CHECK(quote_windows_arg(L"a b") == L"\"a b\"");
  CHECK(quote_windows_arg(L"x\"y") == L"\"x\\\"y\"");
  CHECK(quote_windows_arg(L"C:\\games\\") == L"\"C:\\games\\\\\"");
}
#ifdef _WIN32
#endif

TEST_CASE("debug reports keep the last launch and leave out names, folders and tokens") {
#ifdef _WIN32
  const std::string home = std::getenv("USERPROFILE");
#else
  const std::string home = std::getenv("HOME");
#endif
  std::string slashed = home;
  std::ranges::replace(slashed, '\\', '/');
  const std::string token = "OC" + std::string(60, 'a');
  std::string text = launcher::redact(
      "game " + home + "/a and " + slashed + "/b\n"
      "Polar Bridge by Bri, account 1234567890123456\n"
      "auth " + token + " access_token=abcdefghijkl done\n",
      {"Bri", "1234567890123456"});
  CHECK(text.find(home) == std::string::npos);
  CHECK(text.find("game ~/a and ~/b") != std::string::npos);
  CHECK(text.find("Polar Bridge by <account>, account <account>") != std::string::npos);
  CHECK(text.find(token) == std::string::npos);
  CHECK(text.find("access_token=<removed> done") != std::string::npos);
  CHECK(launcher::redact("OculusAccountIDProvider ready\n", {}) == "OculusAccountIDProvider ready\n");
#ifndef _WIN32
  {
    // rpm-ostree: HOME=/home/<user> while resolved paths read /var/home/<user>.
    const auto base = fs::temp_directory_path() / ("venture-home-" + std::to_string(current_process_id()));
    fs::remove_all(base);
    fs::create_directories(base / "var" / "home" / "pat");
    fs::create_directory_symlink(base / "var" / "home", base / "home");
    const auto real = fs::canonical(base / "var" / "home" / "pat").string();
    setenv("HOME", (base / "home" / "pat").c_str(), 1);
    const auto redacted = launcher::redact("runtime -> " + real + "/.local/share/x\n", {});
    setenv("HOME", home.c_str(), 1);
    fs::remove_all(base);
    CHECK(redacted == "runtime -> ~/.local/share/x\n");
  }
#endif

  auto data = fs::temp_directory_path() / ("venture-report-test-" + std::to_string(current_process_id()));
  fs::remove_all(data);
  fs::create_directories(data / "logs");
  fs::create_directories(data / "accounts");
  std::ofstream(data / "accounts" / "index.json") << "secret-account-file";
  std::ofstream(data / "logs" / "runtime-com.example.old.log") << "old game\n";
  fs::last_write_time(data / "logs" / "runtime-com.example.old.log",
                      fs::file_time_type::clock::now() - std::chrono::hours(1));
  // The conversion report a launch names lists the Android classes the game lacks.
  std::ofstream(data / "report.json")
      << R"({"failures": [{"class": "com.example.Main", "detail": "android/media/audiofx/AudioEffect"},
                          {"class": "com.example.Mic", "detail": "android/media/audiofx/AudioEffect"},
                          {"class": "com.example.Other", "detail": "android/os/ResultReceiver"}]})";
  std::ofstream(data / "logs" / "runtime-com.example.game.log")
      << "started from " << home << "\nW [launch] APK has unresolved dependencies. Details: "
      << paths::utf8(data / "report.json") << "\r\nfailed\n";
  std::ofstream(data / "logs" / "runtime-com.example.game-20260101-000000-1-2.dmp") << "dump bytes";
  std::ofstream(data / "logs" / "runtime-com.example.game-hs_err-77.log")
      << "#\n# A fatal error has been detected by the Java Runtime Environment:\n#\n"
         "#  EXCEPTION_ACCESS_VIOLATION (0xc0000005) at pc=0x00007ffb71c15fe0\n#\n"
         "---------------  T H R E A D  ---------------\n\n"
         "Current thread (0x1): JavaThread \"main\"\n\nRegisters:\nRAX=0x1234\n\n"
         "Native frames: (J=compiled Java code)\nC  [vulkan-1.dll+0x45fe0]\nC  [SomeLayer.dll+0x10]\n\n"
         "---------------  P R O C E S S  ---------------\nDynamic libraries:\n0x1 C:\\x.dll\n";
  std::ofstream(data / "settings.json") << "{\"logLevel\": 2}";
  auto file = launcher::write_debug_report({.data = data, .version = "9.9.9", .private_text = {"secretalias"}});
  std::ifstream in(file, std::ios::binary);
  std::string report((std::istreambuf_iterator<char>(in)), {});
  CHECK(file.parent_path() == data / "reports");
  CHECK(report.find("Venture: 9.9.9") != std::string::npos);
  CHECK(report.find("Game: com.example.game") != std::string::npos);
  CHECK(report.find("started from ~") != std::string::npos);
  CHECK(report.find("runtime-com.example.game-20260101-000000-1-2.dmp") != std::string::npos);
  CHECK(report.find("dump bytes") == std::string::npos);
  // The JVM's crash report: summary and native stack, not registers or modules.
  CHECK(report.find("=== runtime-com.example.game-hs_err-77.log") != std::string::npos);
  CHECK(report.find("EXCEPTION_ACCESS_VIOLATION (0xc0000005)") != std::string::npos);
  CHECK(report.find("Current thread (0x1)") != std::string::npos);
  CHECK(report.find("C  [SomeLayer.dll+0x10]") != std::string::npos);
  CHECK(report.find("RAX=0x1234") == std::string::npos);
  CHECK(report.find("Dynamic libraries") == std::string::npos);
  CHECK(report.find("old game") == std::string::npos);
  CHECK(report.find("secret-account-file") == std::string::npos);
  CHECK(report.find("\"logLevel\": 2") != std::string::npos);
  CHECK(report.find("=== Missing Android classes and members (2 needed by 3 app classes) ===\n"
                    "android/media/audiofx/AudioEffect (2; e.g. com.example.Main)\n") != std::string::npos);
  in.close();

  // Long logs keep their ends and the report stays under the paste service's 1 MB.
  const std::string line(99, 'x');
  for (const char *name : {"runtime-com.example.game.log", "runtime-com.example.game.console.log",
                           "runtime-com.example.game.prev.log", "launcher.log", "launcher-direct-1.log"}) {
    std::ofstream log(data / "logs" / name, std::ios::binary);
    log << "     1.000  42 I [facts] Host runtime: Fixture XR in " << name << '\n';
    for (int i = 0; i < 30000; ++i) log << line << '\n';
    log << "last line of " << name << '\n';
  }
  file = launcher::write_debug_report({.data = data, .version = "9.9.9", .package = "com.example.game"});
  CHECK(fs::file_size(file) < 1'000'000);
  in.open(file, std::ios::binary);
  report.assign(std::istreambuf_iterator<char>(in), {});
  CHECK(report.find("last line of runtime-com.example.game.log") != std::string::npos);
  CHECK(report.find("last line of launcher.log") != std::string::npos);
  // Their starts too, where the session's setup is.
  CHECK(report.find("I [facts] Host runtime: Fixture XR in launcher.log") != std::string::npos);
  CHECK(report.find(" KiB left out ...\n") != std::string::npos);
  // The runtime's facts survive the cut at the log's start.
  CHECK(report.find("=== Session facts ===\n     1.000  42 I [facts] Host runtime: Fixture XR in "
                    "runtime-com.example.game.log") != std::string::npos);
  in.close();
  fs::remove_all(data);
}

TEST_CASE("save data imports find the game's folder and replace what they bring, keeping a backup") {
  auto root = fs::temp_directory_path() / ("venture-saves-test-" + std::to_string(current_process_id()));
  fs::remove_all(root);
  const std::string package = "com.example.game";
  auto write = [](const fs::path& file, std::string_view text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
  };
  // A copy of the headset's /sdcard, a copy of private storage, a files folder.
  auto sdcard = root / "sdcard";
  write(sdcard / "Android/data" / package / "files/save.dat", "imported");
  write(sdcard / "Android/data" / package / "cache/junk", "cache");
  auto found = saves::find_local(sdcard, package);
  REQUIRE(found);
  CHECK(found->area == saves::Area::external);
  CHECK(found->folder == sdcard / "Android/data" / package);
  write(root / "private/shared_prefs/prefs.xml", "<map/>");
  found = saves::find_local(root / "private", package);
  REQUIRE(found);
  CHECK(found->area == saves::Area::internal);
  write(root / "picked/files/slot1.sav", "slot");
  CHECK(saves::find_local(root / "picked" / "files", package)->entry == "files");
  CHECK_FALSE(saves::find_local(root / "sdcard" / "Android", package));

  auto data = root / "data";
  auto game = data / "data/users/local" / package;
  write(game / "external-data/files/save.dat", "current");
  write(game / "external-data/files/extracted.bin", "unity");
  write(game / "external-data/other/keep.txt", "kept");
  auto n = saves::import_local(data, "local", package, *saves::find_local(sdcard, package));
  CHECK(n.files == 1);
  std::ifstream in(game / "external-data/files/save.dat");
  std::string text((std::istreambuf_iterator<char>(in)), {});
  CHECK(text == "imported");
  CHECK_FALSE(fs::exists(game / "external-data/files/extracted.bin"));  // files/ was replaced whole
  CHECK(fs::exists(game / "external-data/other/keep.txt"));
  CHECK_FALSE(fs::exists(game / "external-data/cache"));
  REQUIRE(!n.backup.empty());
  CHECK(fs::exists(n.backup / "Android-data/files/extracted.bin"));
  CHECK(n.backup.parent_path() == saves::backups_dir(data, "local", package));
  // Private storage never brings the folders of the other storage areas.
  write(root / "private/external-data/files/x", "no");
  n = saves::import_local(data, "local", package, *saves::find_local(root / "private", package));
  CHECK(n.files == 1);
  CHECK(fs::exists(game / "shared_prefs/prefs.xml"));
  CHECK(fs::exists(game / "external-data/files/save.dat"));
  in.close();
  fs::remove_all(root);
}

TEST_CASE("an install names only the expansion files its plan requires and lacks") {
  auto dir = fs::temp_directory_path() / ("venture-obb-plan-test-" + std::to_string(current_process_id()));
  fs::create_directories(dir);
  library::Install install;
  install.directory = dir;
  install.plan.package = "com.example.game";
  install.plan.files = {{{}, "game.apk", {}, "apk"},
                        {{}, "main.7.com.example.game.obb", {}, "obb"},
                        {{}, "dlc.obb", {}, "obb", 0, true, "dlc-sku"}};
  std::ofstream(dir / "game.apk") << "apk";
  // The patch OBB Unity probes for is not in the plan, so it is never named.
  CHECK(library::missing_expansions(install) == std::vector<std::string>{"main.7.com.example.game.obb"});
  std::ofstream(dir / "main.7.com.example.game.obb") << "obb";
  CHECK(library::missing_expansions(install).empty());
  fs::remove_all(dir);
}

TEST_CASE("a game that asked Meta for its user without a sign-in is recognised from its log") {
  auto dir = fs::temp_directory_path() / ("venture-signin-test-" + std::to_string(current_process_id()));
  fs::create_directories(dir);
  auto write = [&](const char* name, std::string_view text) {
    std::ofstream(dir / name, std::ios::binary) << text;
    return dir / name;
  };
  CHECK(library::needed_meta_sign_in(write("none.log",
      "  1.0 1 W [ovrplatform] ovr_User_GetLoggedInUser request=1 type=0x436f345d error=-20001\n")));
  CHECK(library::needed_meta_sign_in(write("entitlement.log",
      "  1.0 1 I [launch] Process started\n  2.0 1 W [ovrplatform] ovr_Entitlement_GetIsViewerEntitled "
      "request=2 type=0x186b58b1 error=-20001\n")));
  CHECK_FALSE(library::needed_meta_sign_in(write("other.log",
      "  2.0 1 W [ovrplatform] ovr_User_GetLoggedInUser request=1 type=0x436f345d error=-20003\n"
      "  3.0 1 W [ovrplatform] ovr_Leaderboard_Get request=4 type=0x1 error=-20001\n")));
  CHECK(library::needed_meta_sign_in(write("init.log",
      "  1.0 1 W [ovrplatform] initialization failed error=-20001\n"
      "  2.0 1 I [xr] Frames submitted=1 xrEndFrame_errors=0 submission_fps=0.00 runtime_period_hz=72.00\n")));
  CHECK_FALSE(library::needed_meta_sign_in(write("init-played.log",
      "  1.0 1 W [ovrplatform] initialization failed error=-20001\n"
      "  9.0 1 I [xr] Frames submitted=5400 xrEndFrame_errors=0 submission_fps=72.00 runtime_period_hz=72.00\n")));
  CHECK(library::needed_meta_sign_in(write("init-async.log",
      "  1.0 1 W [ovrplatform] ovr_PlatformInitializeAndroidAsynchronous request=1 type=0x1ad307b4 error=-20001\n"
      "  2.0 1 I [xr] Frames submitted=1 xrEndFrame_errors=0 submission_fps=0.00 runtime_period_hz=72.00\n"
      "  3.0 1 I [launch] Terminal guest process exit status=0\n")));
  const auto played = std::string("  1.0 1 W [ovrplatform] initialization failed error=-20001\n"
      "  60.0 1 I [xr] Frames submitted=4300 xrEndFrame_errors=0 submission_fps=72.00 runtime_period_hz=72.00\n"
      "  61.0 1 W [ovrplatform] ovr_IAP_GetProductsBySKU request=1 type=0x7e9acaf5 error=-20007\n");
  CHECK(library::needed_meta_sign_in(write("init-callback-crash.log", played +
      "  61.2 1003 E [libc] Fatal signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0x148 in tid 1003\n"
      "  61.3 1 I [launch] Terminal guest process exit status=139\n")));
  CHECK_FALSE(library::needed_meta_sign_in(write("init-later-crash.log", played +
      "  900.0 1 I [xr] Frames submitted=60000 xrEndFrame_errors=0 submission_fps=72.00 runtime_period_hz=72.00\n"
      "  901.0 1003 E [libc] Fatal signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0x0 in tid 1003\n")));
  // Renders on, but ends polling for a user it never gets (Pinball FX, #94).
  std::string polling = "  1.0 1 W [ovrplatform] initialization failed error=-20001\n"
      "  20.0 1 I [xr] Frames submitted=2100 xrEndFrame_errors=0 submission_fps=89.71 runtime_period_hz=90.00\n";
  for (int i = 0; i < 250; ++i)
    polling += std::format("  {:.2f} 1 I [ovrplatform] ovr_GetLoggedInUserID caller=0x7f001234 "
                           "identity_returned=false\n", 30.0 + i * 0.2);
  CHECK(library::needed_meta_sign_in(write("init-polling.log", polling)));
  std::string playing = polling;
  for (int i = 0; i < 60; ++i)
    playing += std::format("  {:.1f} 1 I [xr] Frames submitted={} xrEndFrame_errors=0 submission_fps=90.00 "
                           "runtime_period_hz=90.00\n", 90.0 + i, 8000 + i * 90);
  CHECK_FALSE(library::needed_meta_sign_in(write("init-polling-then-played.log", playing)));
  CHECK_FALSE(library::needed_meta_sign_in(dir / "missing.log"));
  const auto missing_line = std::string("  1.0 1 W [vfs] Missing expansion file: the game looked for "
                                        "patch.42.com.example.game.obb, which this install does not have\n");
  const auto unity_error = std::string("  2.0 1 E [Unity] Unable to load GUID for '/storage/emulated/0/Android/obb/"
                                       "com.example.game/patch.42.com.example.game.obb'\n");
  CHECK(library::missing_expansion(write("obb.log", missing_line + unity_error)) == "patch.42.com.example.game.obb");
  CHECK(library::missing_expansion(write("obb-error-first.log", unity_error + missing_line)) ==
        "patch.42.com.example.game.obb");
  // A probe for an optional file the game never complains about is not the cause.
  CHECK(library::missing_expansion(write("obb-probe.log", missing_line)).empty());
  CHECK(library::missing_expansion(write("obb-played.log", missing_line + unity_error +
      "  9.0 1 I [xr] Frames submitted=5400 xrEndFrame_errors=0 submission_fps=72.00 runtime_period_hz=72.00\n")).empty());
  const auto refused = std::string("  100.0 1 W [ovrplatform] ovr_DeviceApplicationIntegrity_GetIntegrityToken "
                                   "request=6 type=0x3271abda error=-20000\n");
  CHECK(library::needed_device_attestation(write("attest.log", refused +
      "Unity stack line\n  111.0 1 I [launch] Terminal guest process exit status=0\n")));
  CHECK_FALSE(library::needed_device_attestation(write("attest-played.log", refused +
      "  900.0 1 I [launch] Terminal guest process exit status=0\n")));
  CHECK_FALSE(library::needed_device_attestation(write("attest-none.log",
      "  1.0 1 I [launch] Process started\n")));
  // Issue 92: Unity's graphics warning, answered by Venture, then the game quit.
  const auto dialog = std::string("  15989.612 79632 W [dialog] Shown without presentation: Warning: Your device does "
                                  "not match the hardware requirements of this application.\n"
                                  "  15989.613 79632 W [dialog] Buttons: Continue, Abort; Venture answers \"Abort\"\n");
  CHECK(library::game_dialog(write("dialog.log", "  15985.0 1 I [launch] Process started\n" + dialog +
        "  15990.0 1 I [launch] Terminal guest process exit status=1\n")) ==
        "Warning: Your device does not match the hardware requirements of this application.");
  // A game that played on after a dialog, or showed none, was not closed by one.
  CHECK(library::game_dialog(write("dialog-played.log", dialog +
        "  15999.0 1 I [xr] Frames submitted=720 xrEndFrame_errors=0 submission_fps=72.00 runtime_period_hz=72.00\n"
        "  16000.0 1 I [launch] Terminal guest process exit status=0\n")).empty());
  CHECK(library::game_dialog(write("dialog-late.log", dialog +
        "  16100.0 1 I [launch] Terminal guest process exit status=0\n")).empty());
  CHECK(library::game_dialog(write("dialog-none.log", "  1.0 1 I [launch] Process started\n")).empty());
  fs::remove_all(dir);
}

TEST_CASE("a runtime that ends on a fatal signal reports the signal and its trap, not a later error line") {
  // Issue 58: Unity's non-fatal HTTPS error was logged after the crash that ended the run.
  const std::string ssl = "  5.9 1 E [Unity] javax.net.ssl.SSLHandshakeException: (handshake_failure) Received fatal alert\n";
  const std::string trap = "  8.5 2 E [a32-abi] unsupported libmediandk.so!AMediaExtractor_new() -> unknown; "
                           "descriptor missing; adapter required\n";
  const std::string signal = "  8.5 2 E [libc] Fatal signal 31 (SIGSYS), code 1 (SYS_SECCOMP) in tid 1013, pid 1000 (game)\n"
                             "  8.5 2 E [signal]     r0  f7fa7540  r1  04772de0\n";
  auto failure = library::runtime_failure(ssl + trap + signal + ssl, 159);
  CHECK(failure == "The runtime stopped unexpectedly (exit code 159). Last error: unsupported "
                   "libmediandk.so!AMediaExtractor_new() -> unknown; descriptor missing; adapter required "
                   "(Fatal signal 31 (SIGSYS), code 1 (SYS_SECCOMP) in tid 1013, pid 1000 (game))");
  CHECK(library::runtime_failure(ssl + "  9.0 2 E [hle] unimplemented libfoo.so!bar called from x+0x1 (y+0x1)\n" +
                                     signal, 159).find("Last error: unimplemented libfoo.so!bar") != std::string::npos);
  // A plain crash names its signal; a trap logged before an earlier, handled signal is not the cause.
  const std::string segv = "  9.0 3 E [libc] Fatal signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0x0 in tid 1003\n";
  CHECK(library::runtime_failure(ssl + segv + ssl, 139) ==
        "The runtime stopped unexpectedly (exit code 139). Last error: Fatal signal 11 (SIGSEGV), code 1 "
        "(SEGV_MAPERR), fault addr 0x0 in tid 1003");
  CHECK(library::runtime_failure(trap + signal + segv, 139).ends_with("Last error: Fatal signal 11 (SIGSEGV), code 1 "
                                                                       "(SEGV_MAPERR), fault addr 0x0 in tid 1003"));
  // A guest crash handler that re-raises logs no libc line: the guest fault summary for the exit's signal names it.
  const std::string unity = "  7.0 1 E [signal] guest tid 1000 signal 11 code 1 at PC 0x0, fault 0x0\n"
                            "  7.0 1 E [CRASH] Cause: null pointer dereference\n"
                            "  7.1 1 E [signal]   exidx unwind stopped: EXIDX_CANTUNWIND\n";
  CHECK(library::runtime_failure(ssl + unity, 139).ends_with("Last error: guest tid 1000 signal 11 code 1 at PC 0x0, fault 0x0"));
  CHECK(library::runtime_failure(ssl + unity, 1).ends_with("exidx unwind stopped: EXIDX_CANTUNWIND"));
  // A re-raised SIGSYS keeps the unimplemented import that raised it; other signals do not take a trap as cause.
  const std::string missing = "  6.0 1 E [hle] unimplemented libfoo.so!bar called from x+0x1 (y+0x1)\n";
  const std::string sys = "  6.1 1 E [signal] guest tid 1000 signal 31 code 1 at PC 0x1618, fault 0x1614, "
                          "terminated by the default action\n";
  CHECK(library::runtime_failure(missing + sys + ssl, 159).ends_with(
      "Last error: unimplemented libfoo.so!bar called from x+0x1 (y+0x1) (guest tid 1000 signal 31 code 1 at PC "
      "0x1618, fault 0x1614, terminated by the default action)"));
  CHECK(library::runtime_failure(missing + unity, 139).ends_with("Last error: guest tid 1000 signal 11 code 1 at PC 0x0, fault 0x0"));
  // Without a fatal signal the last error line stays the context.
  CHECK(library::runtime_failure(ssl, 1).ends_with("Last error: javax.net.ssl.SSLHandshakeException: "
                                                   "(handshake_failure) Received fatal alert"));
  // A game whose VR session was not created crashes later on the invalid
  // handle (Batman: Arkham Shadow aborted in OVRPlugin); the session is the cause.
  const std::string session = "  9.2 1 E [xr] The VR session could not be created: Oculus refused it "
                              "(XR_ERROR_GRAPHICS_DEVICE_INVALID)\n";
  const std::string abort = "  9.9 1 E [libc] Fatal signal 6 (SIGABRT), code -1 (SI_QUEUE) in tid 1001\n";
  CHECK(library::runtime_failure(session + ssl + abort, 134) ==
        "The VR session could not be created: Oculus refused it (XR_ERROR_GRAPHICS_DEVICE_INVALID). "
        "The game stopped after that (exit code 134).");
  CHECK(library::runtime_failure(session, -1073741819).ends_with("(exit code 0xC0000005)."));
  // A session created after the failure supersedes it.
  CHECK(library::runtime_failure(session + "  9.5 1 I [xr] Host xrCreateSession result=0\n" + abort, 134) ==
        "The runtime stopped unexpectedly (exit code 134). Last error: Fatal signal 6 (SIGABRT), code -1 "
        "(SI_QUEUE) in tid 1001");
}

TEST_CASE("a game closed while hung reports that it stopped responding, not a stall snapshot line") {
  // A user report (Max Mustard, 0.1.4.2): the game froze, the user closed it, and the
  // launcher quoted the stall snapshot's last thread row as "Last error".
  const std::string frames = "  667.3 1 I [xr] Frames submitted=2100 xrEndFrame_errors=0 submission_fps=83.36 "
                             "runtime_period_hz=90.00\n";
  const std::string header = "  672.8 9 E [startup-snapshot] No frame submitted for 5 s while the game has focus; "
                             "capturing thread state\n";
  const std::string rows = "  672.8 9 E [startup-snapshot] raw guest stacks reason=frame-stall registry_available=true "
                           "dropped=0 state=last-saved\n"
                           "  672.8 9 E [startup-snapshot] guest tid=1035 pc=0x7ef43b6a27d8 (host call "
                           "libvulkan.so!vkWaitForFences) lr=0x7ef43d16038c sp=0x7ef4fc900a00\n"
                           "  672.8 9 E [startup-snapshot] guest tid=1055 pc=0x0 lr=0x0 sp=0x7ef52fc8c000\n";
  const std::string freeze = "  697.8 9 E [startup-snapshot] No frame submitted for 30 s while the game has focus; "
                             "capturing thread state and a native dump\n";
  const std::string unity = "  697.8 9 E [Unity] A scripted object has a different serialization layout when loading.\n";
  const std::string quit = "  15672.8 9 I [launch] Shutdown requested; posting Android lifecycle teardown\n"
                           "  15683.2 9 W [launch] Stopping app worker after shutdown request or teardown grace expired\n";
  CHECK(library::runtime_failure(frames + unity + header + rows + quit, 124) ==
        "The game stopped responding (no new frame for 5 s) and was closed (exit code 124).");
  CHECK(library::runtime_failure(frames + header + rows + unity + freeze + rows, -1) ==
        "The game stopped responding (no new frame for 30 s) and was closed (exit code 0xFFFFFFFF).");
  // The snapshot can be longer than the log tail the launcher reads: its rows alone still mean a hang.
  CHECK(library::runtime_failure(rows, 124) == "The game stopped responding and was closed (exit code 124).");
  // Frames after a stall (a long level load): the run did not end hung, and snapshot rows are never the error.
  CHECK(library::runtime_failure(header + rows + frames + unity, 1) ==
        "The runtime stopped unexpectedly (exit code 1). Last error: A scripted object has a different "
        "serialization layout when loading.");
  CHECK(library::runtime_failure(header + rows + frames, 1) == "The runtime stopped unexpectedly (exit code 1).");
  // A crash outranks the stall.
  const std::string segv = "  700.0 3 E [libc] Fatal signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0x0 in tid 1003\n";
  CHECK(library::runtime_failure(header + rows + segv, 139).ends_with("Last error: Fatal signal 11 (SIGSEGV), code 1 "
                                                                       "(SEGV_MAPERR), fault addr 0x0 in tid 1003"));
}

TEST_CASE("a HotSpot fatal error report names the native crash like Venture's crash reporter") {
  // AC Nexus on AMD: a crash in the Vulkan loader on Unity's Java render thread
  // went to HotSpot's handler, which exits 1; the launcher showed a missing
  // library logged earlier as the reason.
  const std::string windows =
      "#\n# A fatal error has been detected by the Java Runtime Environment:\n#\n"
      "#  EXCEPTION_ACCESS_VIOLATION (0xc0000005) at pc=0x00007ff890c486d0, pid=16840, tid=32948\r\n#\n"
      "# JRE version: OpenJDK Runtime Environment Temurin-21.0.12.1+1 (21.0.12.1+1)\n"
      "# Problematic frame:\n# C  [vulkan-1.dll+0x486d0]\n#\n"
      "---------------  T H R E A D  ---------------\n\n"
      "Current thread (0x00000264a1392dc0):  JavaThread \"UnityMain\"        [_thread_in_native, id=32948]\n";
  CHECK(venture::hotspot_fatal_summary(windows) ==
        "Host exception 0xc0000005 at vulkan-1.dll+0x486d0 (Java thread UnityMain)");
  const std::string linux_report =
      "# A fatal error has been detected by the Java Runtime Environment:\n"
      "#  SIGSEGV (0xb) at pc=0x00007f3c2a3b4000, pid=4242, tid=4250\n"
      "# Problematic frame:\n# C  [libvulkan.so.1+0x2a3b4]  vkQueueSubmit+0x14\n"
      "Current thread (0x00007f3c00001000):  VMThread \"VM Thread\" [stack: 0x1,0x2] [id=4250]\n";
  CHECK(venture::hotspot_fatal_summary(linux_report) ==
        "Host exception 0xb at libvulkan.so.1+0x2a3b4 (vkQueueSubmit+0x14) (VMThread \"VM Thread\")");
  CHECK(venture::hotspot_fatal_summary(
            "# A fatal error has been detected by the Java Runtime Environment:\n"
            "#  Internal Error (g1PageBasedVirtualSpace.cpp:42), pid=1, tid=2\n") ==
        "Java runtime fatal error: Internal Error (g1PageBasedVirtualSpace.cpp:42), pid=1, tid=2");
  CHECK(venture::hotspot_fatal_summary(
            "#\n# There is insufficient memory for the Java Runtime Environment to continue.\n"
            "# Native memory allocation (mmap) failed to map 65536 bytes.\n") ==
        "Java runtime fatal error: There is insufficient memory for the Java Runtime Environment to continue.");
  CHECK(venture::hotspot_fatal_summary("  1.0 1 E [linker] library not found: libMemoryPolling.so\n").empty());
}

TEST_CASE("updates pick this platform's release file and its checksum") {
  using namespace venture::launcher;
  const auto release = Json::parse(R"({"assets": [
    {"name": "SHA256SUMS", "size": 300, "browser_download_url": "https://github.com/Villagers654/Venture/releases/download/v9/SHA256SUMS"},
    {"name": "Venture-9-linux-x86_64.AppImage", "size": 7, "browser_download_url": "https://github.com/Villagers654/Venture/releases/download/v9/Venture-9-linux-x86_64.AppImage"},
    {"name": "Venture-windows64-setup.exe", "size": 9, "browser_download_url": "https://evil.example/Venture-windows64-setup.exe"},
    {"name": "Venture-windows64.zip", "size": 8, "browser_download_url": "https://github.com/Villagers654/Venture/releases/download/v9/Venture-windows64.zip"}]})");
  const auto image = update::pick(release, update::Method::appimage);
  REQUIRE(image);
  CHECK(image->name == "Venture-9-linux-x86_64.AppImage");
  CHECK(image->size == 7);
  // Only GitHub serves Venture's releases.
  CHECK_FALSE(update::pick(release, update::Method::installer));
  CHECK_FALSE(update::pick(release, update::Method::page));
  CHECK(update::sums(release));
  const std::string a(64, 'A'), b(64, 'b');
  const auto text = a + "  Venture-windows64-setup.exe\r\n" + b + " *Venture-9-linux-x86_64.AppImage\n" + "zz  bad\n";
  CHECK(update::expected_sum(text, "Venture-windows64-setup.exe") == std::string(64, 'a'));
  CHECK(update::expected_sum(text, "Venture-9-linux-x86_64.AppImage") == b);
  CHECK(update::expected_sum(text, "Venture-windows64.zip").empty());
}

TEST_CASE("store catalogs keep Meta's asset type and name language packs by file") {
  auto b = binary();
  auto &nodes = b["asset_files"]["nodes"];
  nodes[0]["asset_file_type"] = "DEFAULT";
  nodes[1]["asset_file_type"] = "LANGUAGE_PACK";
  nodes[1]["file_name"] = "ko.lang";
  nodes[2]["asset_file_type"] = "LANGUAGE_PACK";
  nodes[2]["file_name"] = "voices_all.pak";
  auto plan = meta::build_plan("123456", b);
  auto file = [&](std::string_view name) { return *std::ranges::find(plan.files, name, &meta::File::name); };
  CHECK(file("required.dat").asset_type == "default");
  CHECK(file("required.dat").language_tag.empty());
  // Asgard's Wrath 2 names its packs ko.lang; Meta lists no tag for them.
  CHECK(file("ko.lang").asset_type == "language_pack");
  CHECK(file("ko.lang").language_tag == "ko");
  CHECK(file("voices_all.pak").asset_type == "language_pack");
  CHECK(file("voices_all.pak").language_tag.empty());
  auto saved = meta::parse_plan(plan.json());
  CHECK(std::ranges::find(saved.files, "ko.lang", &meta::File::name)->language_tag == "ko");
}
