// Store links of imported APKs (launcher/store_link.h): link parsing, the
// plan.json field, the package check and the order lookups are tried in.
#include "launcher/store_link.h"

#include "common/meta/store.h"
#include <doctest/doctest.h>
#include <stdexcept>
#include <map>
#include <vector>

using namespace venture;
namespace sl = venture::launcher::store_link;

TEST_CASE("store links take the app id from a store URL or a bare id") {
  CHECK(sl::id_from_link("https://www.meta.com/experiences/3551691271620960/") == "3551691271620960");
  CHECK(sl::id_from_link("meta.com/experiences/3551691271620960") == "3551691271620960");
  CHECK(sl::id_from_link("https://www.oculus.com/experiences/quest/3551691271620960/?locale=en_US") ==
        "3551691271620960");
  CHECK(sl::id_from_link("https://www.meta.com/en-gb/experiences/batman-arkham-shadow/3551691271620960/?utm=1") ==
        "3551691271620960");
  CHECK(sl::id_from_link("  3551691271620960 ") == "3551691271620960");
  CHECK(sl::id_from_link("https://www.meta.com/experiences/3551691271620960/?utm_id=123456") == "3551691271620960");
  // Short runs of digits (a locale, a version) are not app ids.
  CHECK(sl::id_from_link("https://www.meta.com/experiences/").empty());
  CHECK(sl::id_from_link("version 1234").empty());
  CHECK(sl::id_from_link("").empty());
}

TEST_CASE("a local import's plan keeps its store link, and plans without one stay valid") {
  Json j = {{"appId", ""},
            {"binaryId", ""},
            {"source", "import"},
            {"package", "com.example.game"},
            {"version", "1.0"},
            {"versionCode", 7},
            {"files", Json::array({{{"id", ""}, {"name", "com.example.game.apk"}, {"size", 10}, {"kind", "apk"}}})},
            {"installedAddons", Json::array()}};
  auto plan = meta::parse_plan(j);
  CHECK(plan.local);
  CHECK(plan.app_id == "com.example.game");
  CHECK(plan.store_app_id.empty());
  CHECK_FALSE(plan.store_link_removed);
  CHECK_FALSE(plan.json().contains("storeAppId"));

  plan.store_app_id = "3551691271620960";
  auto written = plan.json();
  CHECK(written["appId"] == "");  // the install keeps its key
  CHECK(written["storeAppId"] == "3551691271620960");
  auto again = meta::parse_plan(written);
  CHECK(again.app_id == "com.example.game");
  CHECK(again.store_app_id == "3551691271620960");

  // Removed by the user: remembered, so it is not looked up again.
  again.store_app_id.clear();
  again.store_link_removed = true;
  written = again.json();
  CHECK_FALSE(written.contains("storeAppId"));
  CHECK(written["storeLinkRemoved"] == true);
  CHECK(meta::parse_plan(written).store_link_removed);

  // A link that is not an app id is ignored; the plan still loads.
  j["storeAppId"] = "not-an-id";
  auto odd = meta::parse_plan(j);
  CHECK(odd.store_app_id.empty());
  // Store installs (a numeric app id) never carry one.
  j["appId"] = "1234567890";
  j["storeAppId"] = "3551691271620960";
  CHECK(meta::parse_plan(j).store_app_id.empty());
}

TEST_CASE("a store link is accepted only for a listing of the APK's package") {
  CHECK(sl::listing_matches("com.camouflaj.manta", "com.camouflaj.manta"));
  CHECK_FALSE(sl::listing_matches("com.camouflaj.manta.beta", "com.camouflaj.manta"));
  CHECK_FALSE(sl::listing_matches("com.Camouflaj.Manta", "com.camouflaj.manta"));
  CHECK_FALSE(sl::listing_matches("", "com.camouflaj.manta"));
  CHECK_FALSE(sl::listing_matches("", ""));
}

TEST_CASE("OculusDB answers name the app id only for the package asked about") {
  CHECK(sl::oculusdb_id(R"({"id":"3551691271620960","displayName":"Batman: Arkham Shadow",)"
                        R"("packageName":"com.camouflaj.manta"})",
                        "com.camouflaj.manta") == "3551691271620960");
  CHECK(sl::oculusdb_id(R"([{"id":"111111","packageName":"other"},{"id":"222222","packageName":"com.a.b"}])",
                        "com.a.b") == "222222");
  CHECK(sl::oculusdb_id(R"({"id":"3551691271620960","packageName":"com.other"})", "com.camouflaj.manta").empty());
  CHECK(sl::oculusdb_id(R"({"id":"12","packageName":"com.a.b"})", "com.a.b").empty());
  CHECK(sl::oculusdb_id("not json", "com.a.b").empty());
  CHECK(sl::oculusdb_id("{}", "com.a.b").empty());
}

TEST_CASE("the shipped package pairs come from the compatibility list") {
  CHECK(sl::known_id("com.camouflaj.manta") == "3551691271620960");
  CHECK(sl::known_id("com.example.not.listed").empty());
  CHECK(sl::known_id("").empty());
}

TEST_CASE("store link lookups go library, shipped pairs, then online, each checked against the listing") {
  std::vector<std::string> calls;
  std::map<std::string, std::string> listings = {{"1000001", "com.a.b"}, {"2000002", "com.a.b"},
                                                 {"3000003", "com.other"}};
  sl::Lookups l;
  l.listing_package = [&](std::string_view id) {
    calls.push_back("listing " + std::string(id));
    auto it = listings.find(std::string(id));
    if (it == listings.end()) throw std::runtime_error("no listing");
    return it->second;
  };
  l.known = [&](std::string_view) {
    calls.push_back("known");
    return std::string("1000001");
  };
  l.online = [&](std::string_view) {
    calls.push_back("online");
    return std::string("2000002");
  };

  SUBCASE("a signed-in library's game needs no lookup") {
    l.library = "9000009";
    auto r = sl::resolve("com.a.b", l);
    CHECK(r.id == "9000009");
    CHECK(r.source == "library");
    CHECK(calls.empty());
  }
  SUBCASE("a shipped pair is verified and wins before the online lookup") {
    auto r = sl::resolve("com.a.b", l);
    CHECK(r.id == "1000001");
    CHECK(r.source == "known");
    CHECK(calls == std::vector<std::string>{"known", "listing 1000001"});
  }
  SUBCASE("a candidate whose listing is another package is discarded") {
    l.known = [&](std::string_view) {
      calls.push_back("known");
      return std::string("3000003");
    };
    auto r = sl::resolve("com.a.b", l);
    CHECK(r.id == "2000002");
    CHECK(r.source == "online");
    CHECK(calls == std::vector<std::string>{"known", "listing 3000003", "online", "listing 2000002"});
  }
  SUBCASE("with the online lookup off, nothing leaves but the listing check") {
    l.known = [&](std::string_view) {
      calls.push_back("known");
      return std::string();
    };
    l.online = nullptr;
    auto r = sl::resolve("com.a.b", l);
    CHECK(r.id.empty());
    CHECK(r.final);
    CHECK(calls == std::vector<std::string>{"known"});
  }
  SUBCASE("nothing found is final; a failed lookup is not") {
    l.known = [](std::string_view) { return std::string(); };
    l.online = [](std::string_view) { return std::string(); };
    CHECK(sl::resolve("com.a.b", l).final);
    l.online = [](std::string_view) -> std::string { throw std::runtime_error("offline"); };
    auto r = sl::resolve("com.a.b", l);
    CHECK(r.id.empty());
    CHECK_FALSE(r.final);
    // A listing that cannot be read does not link either.
    l.online = [](std::string_view) { return std::string("4000004"); };
    r = sl::resolve("com.a.b", l);
    CHECK(r.id.empty());
    CHECK_FALSE(r.final);
  }
}
