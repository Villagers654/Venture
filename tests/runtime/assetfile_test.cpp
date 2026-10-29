#include <algorithm>

#include "../http_loopback.h"
#include "common/download/asset_files.h"
#include "common/meta/accounts.h"
#include "common/process.h"
#include "hle/ovrplatform/asset_service.h"
#include "hle/ovrplatform/broker.h"
#include "ovrplatform_support.h"

#include "../async_wait.h"

using namespace venture::ovrplatform;
namespace optest = venture::ovrplatform::test;
namespace {
struct Fixture {
  venture::fs::path root =
      venture::fs::temp_directory_path() /
      ("venture-asset-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  AppIdentity app{"123456", "com.example.assets", "654321", 12};
  venture::meta::Plan plan{
      app.app_id,
      app.binary_id,
      app.package,
      "1",
      "12",
      {{"111111", "com.example.assets.apk", {}, "apk", 4},
       {"222222", "required.bundle", {}, "asset", 3},
       {"333333", "optional.bundle", {}, "asset", 5, false},
       {"444444", "paid.bundle", {}, "asset", 4, true, "ride", "888888"},
       {"555555", "fr.bundle", {}, "asset", 2, false}}};
  venture::fs::path install = root / "games" / app.package / "12";
  Fixture() {
    plan.files.back().asset_type = "language_pack";
    plan.files.back().language_tag = "fr-FR";
    plan.files.back().language_english_name = "French";
    plan.files.back().language_native_name = "Français";
    plan.files.back().metadata = "developer metadata";
    plan.installed_addons = {
        "ride"};  // This marker is never ownership evidence.
    venture::paths::atomic_write(install / "plan.json", plan.json().dump());
    venture::paths::atomic_write(install / "com.example.assets.apk", "base");
    venture::paths::atomic_write(install / "required.bundle", "abc");
    venture::paths::atomic_write(install / "paid.bundle", "paid");
  }
  ~Fixture() {
    std::error_code ec;
    venture::fs::remove_all(root, ec);
  }
  std::shared_ptr<AssetService> service(
      AssetOwnershipReader ownership = {},
      std::shared_ptr<venture::PipeChannel> channel = {}) {
    return std::make_shared<AssetService>(
        app, install, "/sdcard/Android/obb/" + app.package,
        std::move(ownership), std::move(channel), root / "state/language.json", root / "view");
  }
};
std::string string_field(const Model& m, std::string_view key) {
  return std::get<std::string>(m.fields.at(std::string(key)));
}
Result query(const std::shared_ptr<AssetService>& s, std::string_view op,
             RequestArg arg) {
  return s->request(op, {{std::move(arg)}}, {});
}
class TokenProvider : public Provider {
 public:
  Error prepare() override { return {}; }
  Result entitlement(std::stop_token) override { return {}; }
  Result access_token(std::stop_token) override {
    return revoked ? Result{failure(ErrorCode::identity_mismatch)}
                   : Result{{}, {}, "fixture-app-token"};
  }
  bool revoked = false;
};
venture::meta::Store::Transport store_queries(const Fixture& f) {
  return [&f](std::string_view url, const venture::http::Fields& fields) {
    if (venture::http::parse_url(url).path == "/authenticate_application")
      return venture::Json{{"access_token", "fixture-app-token"}};
    std::map<std::string, std::string> args(fields.begin(), fields.end());
    if (args["doc_id"] == "4850747515044496")
      return venture::parse_json(
          R"({"data":{"viewer":{"user":{"id":"42","active_entitlements":{"nodes":[{"item":{"id":"123456","platform":"ANDROID_6DOF"}}],"page_info":{"has_next_page":false}}}}}})");
    venture::Json files = venture::Json::array();
    for (auto& file : f.plan.files)
      if (file.kind == "asset") {
        files.push_back(
            {{"id", file.id},
             {"file_name", file.name},
             {"size", file.size},
             {"is_required", file.required},
             {"asset_type", file.asset_type},
             {"metadata", file.metadata},
             {"language",
              {{"tag", file.language_tag},
               {"english_name", file.language_english_name},
               {"native_name", file.language_native_name}}},
             {"iap_item", file.sku.empty() ? venture::Json{}
                                           : venture::Json{{"id", file.iap_id},
                                                      {"sku", file.sku}}}});
      }
    return venture::Json{{"data",
                     {{"node",
                       {{"id", f.app.binary_id},
                        {"binary_application", {{"id", f.app.app_id}}},
                        {"package_name", f.app.package},
                        {"version_code", 12},
                        {"version", "1"},
                        {"size", 4},
                        {"asset_files", files}}}}}};
  };
}
struct BrokerFixture {
  venture::test::Server server;
  std::shared_ptr<HttpTransport> http = loopback_http_transport(server.port);
  std::shared_ptr<CredentialBroker> broker;
  BrokerFixture(const Fixture& f, const std::string& key) {
    server.push(R"({"access_token":"fixture-app-token"})");
    server.push(R"({"id":"123456","alias":"fixture"})");
    // Entitlement is queried only when the game asks (after prepare).
    server.push(store_queries(f)("https://fixture/graphql",
                                {{"doc_id", "4850747515044496"}}).dump());
    auto store = std::make_shared<optest::FakeStore>();
    store->value.access_token = "fixture-session";
    broker = std::make_shared<CredentialBroker>(f.app, key, store, http);
    REQUIRE_FALSE(broker->prepare());
  }
  venture::PipeChannel::Handler route(venture::PipeChannel::Handler assets) {
    return [broker = broker, assets = std::move(assets)](const venture::Json& request) {
      return venture::str(request, "op").starts_with("asset.")
                 ? assets(request) : broker->handle(request);
    };
  }
};
struct CloseBridge {
  std::shared_ptr<venture::PipeChannel> client;
  std::jthread& bridge;
  ~CloseBridge() {
    client->close();
    bridge.request_stop();
    bridge.join();
  }
};
}  // namespace
TEST_CASE(
    "AssetFile hides unowned installed assets and reports real local status") {
  Fixture f;
  auto s = f.service([](std::stop_token) { return AssetOwnership{{}, {}}; });
  CHECK(s->request("LanguageGetCurrent", {}, {}).error);
  auto list = s->request("GetList", {}, {});
  REQUIRE_FALSE(list.error);
  REQUIRE(list.model.elements.size() == 3);
  CHECK(string_field(list.model.elements[0], "DownloadStatus") == "installed");
  CHECK(string_field(list.model.elements[0], "Filepath") ==
        "/sdcard/Android/obb/com.example.assets/required.bundle");
  CHECK(string_field(list.model.elements[1], "DownloadStatus") == "available");
  CHECK(string_field(list.model.elements[1], "Filepath").empty());
  CHECK(query(s, "StatusById", std::uint64_t{444444}).error.code ==
        ErrorCode::not_entitled);
  CHECK(query(s, "StatusByName", std::string("paid.bundle")).error.code ==
        ErrorCode::not_entitled);
  CHECK(query(s, "DownloadByName", std::string("paid.bundle")).error.code ==
        ErrorCode::not_entitled);
  CHECK(query(s, "DeleteByName", std::string("paid.bundle")).error.code ==
        ErrorCode::not_entitled);
  CHECK(venture::fs::exists(f.install / "paid.bundle"));
  auto needed = query(s, "DownloadByName", std::string("optional.bundle"));
  CHECK(needed.error.message == "Venture: asset download requires launcher");
  CHECK_FALSE(query(s, "DeleteById", std::uint64_t{222222}).error);
  CHECK(string_field(query(s, "Status", std::uint64_t{222222}).model,
                     "DownloadStatus") == "available");
  CHECK(venture::fs::exists(f.install / "com.example.assets.apk"));
  auto short_file = f.install / "optional.bundle";
  venture::paths::atomic_write(short_file, "1234");
  CHECK(string_field(
            query(s, "StatusByName", std::string("optional.bundle")).model,
            "DownloadStatus") == "available");
  venture::paths::atomic_write(short_file, "12345");
  CHECK(string_field(
            query(s, "StatusByName", std::string("optional.bundle")).model,
            "DownloadStatus") == "installed");
}
TEST_CASE("AssetFile C API aliases models lifetime and language selection") {
  Fixture f;
  venture::paths::atomic_write(f.install / "fr.bundle", "fr");
  auto service =
      f.service([](std::stop_token) { return AssetOwnership{{}, {"ride"}}; });
  optest::Arena arena;
  Session session(f.app, std::make_shared<optest::FakeProvider>(),
                  arena.memory());
  register_asset_handlers(session, service);
  REQUIRE(session.initialize(f.app.app_id) == InitResult::success);
  for (auto name : {"ovr_AssetFile_Status", "ovr_AssetFile_StatusById",
                    "ovr_AssetFile_StatusByName"}) {
    const char* filename = "required.bundle";
    auto id = optest::invoke(
        session, name,
        {std::string_view(name).ends_with("ByName") ? venture::h2g(filename)
                                                    : 222222});
    auto message = optest::wait_pop(session);
    CHECK(session.get(message, "ovr_Message_GetRequestID") == id);
    CHECK_FALSE(session.get(message, "ovr_Message_IsError"));
    auto asset = session.get(message, "ovr_Message_GetAssetDetails");
    CHECK(session.get(asset, "ovr_AssetDetails_GetAssetId") == 222222);
    session.free_message(message);
    CHECK(session.get(asset, "ovr_AssetDetails_GetAssetId") == 0);
  }
  const char* tag = "fr-FR";
  auto req =
      optest::invoke(session, "ovr_LanguagePack_SetCurrent", {venture::h2g(tag)});
  bool complete = false, notification = false;
  while (!complete) {
    auto msg = optest::wait_pop(session);
    if (!session.get(msg, "ovr_Message_GetRequestID")) {
      CHECK(session.get(msg, "ovr_Message_GetType") == kAssetDownloadUpdate);
      auto update = session.get(msg, "ovr_Message_GetAssetFileDownloadUpdate");
      CHECK(session.get(update, "ovr_AssetFileDownloadUpdate_GetCompleted") ==
            1);
      notification = true;
    } else {
      CHECK(session.get(msg, "ovr_Message_GetRequestID") == req);
      CHECK_FALSE(session.get(msg, "ovr_Message_IsError"));
      complete = true;
    }
    session.free_message(msg);
  }
  CHECK(notification);
  optest::invoke(session, "ovr_LanguagePack_GetCurrent");
  auto msg = optest::wait_pop(session);
  auto asset = session.get(msg, "ovr_Message_GetAssetDetails");
  auto language = session.get(asset, "ovr_AssetDetails_GetLanguage");
  CHECK(std::string(venture::g2h<const char>(
            session.get(language, "ovr_LanguagePackInfo_GetTag"))) == "fr-FR");
  session.free_message(msg);
  CHECK(arena.live == 0);
  auto next = f.service();
  CHECK_FALSE(next->request("LanguageGetCurrent", {}, {}).error);
}
TEST_CASE(
    "AssetFile C API download cancel delete aliases and list use exact SDK "
    "payloads") {
  Fixture f;
  optest::Arena arena;
  Session session(f.app, std::make_shared<optest::FakeProvider>(),
                  arena.memory());
  register_asset_handlers(session, f.service());
  REQUIRE(session.initialize(f.app.app_id) == InitResult::success);
  auto call = [&](std::string_view name, std::string_view getter) {
    const char* filename = "required.bundle";
    auto request = optest::invoke(
        session, name, {name.ends_with("ByName") ? venture::h2g(filename) : 222222});
    for (;;) {
      auto msg = optest::wait_pop(session);
      if (session.get(msg, "ovr_Message_GetRequestID") == request) {
        CHECK_FALSE(session.get(msg, "ovr_Message_IsError"));
        CHECK(session.get(msg, "ovr_Message_GetType") ==
              optest::spec(name).type);
        auto model = session.get(msg, getter);
        CHECK(model != 0);
        session.free_message(msg);
        break;
      }
      CHECK(session.get(msg, "ovr_Message_GetType") == kAssetDownloadUpdate);
      session.free_message(msg);
    }
  };
  for (auto name : {"ovr_AssetFile_Download", "ovr_AssetFile_DownloadById",
                    "ovr_AssetFile_DownloadByName"})
    call(name, "ovr_Message_GetAssetFileDownloadResult");
  for (auto name :
       {"ovr_AssetFile_DownloadCancel", "ovr_AssetFile_DownloadCancelById",
        "ovr_AssetFile_DownloadCancelByName"})
    call(name, "ovr_Message_GetAssetFileDownloadCancelResult");
  for (auto name : {"ovr_AssetFile_Delete", "ovr_AssetFile_DeleteById",
                    "ovr_AssetFile_DeleteByName"}) {
    venture::paths::atomic_write(f.install / "required.bundle", "abc");
    call(name, "ovr_Message_GetAssetFileDeleteResult");
    CHECK_FALSE(venture::fs::exists(f.install / "required.bundle"));
  }
  optest::invoke(session, "ovr_AssetFile_GetList");
  auto msg = optest::wait_pop(session);
  auto list = session.get(msg, "ovr_Message_GetAssetDetailsArray");
  CHECK(session.get(list, "ovr_AssetDetailsArray_GetSize") == 3);
  session.free_message(msg);
  CHECK(arena.live == 0);
}
TEST_CASE("AssetFile serves an import's own files by name, installed, with stable ids") {
  Fixture f;
  // An import without Meta: a local plan whose files carry no asset ids.
  f.plan.local = true;
  for (auto& file : f.plan.files) file.id.clear();
  f.plan.app_id = f.app.package;
  f.plan.binary_id.clear();
  venture::paths::atomic_write(f.install / "plan.json", f.plan.json().dump());
  f.app.app_id.clear();
  f.app.binary_id.clear();
  auto s = f.service();
  auto status = query(s, "StatusByName", std::string("required.bundle"));
  REQUIRE_FALSE(status.error);
  CHECK(string_field(status.model, "DownloadStatus") == "installed");
  CHECK(string_field(status.model, "Filepath") == "/sdcard/Android/obb/com.example.assets/required.bundle");
  auto id = std::get<std::uint64_t>(status.model.fields.at("AssetId"));
  CHECK(id >= (1ull << 63));
  // The same id again, and it downloads (already installed) by id.
  CHECK(std::get<std::uint64_t>(f.service()->request("StatusByName", {{std::string("required.bundle")}}, {})
                                    .model.fields.at("AssetId")) == id);
  CHECK_FALSE(query(s, "DownloadById", id).error);
  CHECK(string_field(query(s, "StatusByName", std::string("optional.bundle")).model, "DownloadStatus") ==
        "available");
}

TEST_CASE("AssetFile reports reduced-catalog assets as entitled") {
  Fixture f;
  auto iap = [&](std::string_view name) {
    auto s = f.service();
    return string_field(query(s, "StatusByName", std::string(name)).model, "IapStatus");
  };
  CHECK(iap("optional.bundle") == "free");
  // Without Meta's full catalog an asset's add-on is unknown.
  f.plan.assets_complete = false;
  venture::paths::atomic_write(f.install / "plan.json", f.plan.json().dump());
  CHECK(iap("optional.bundle") == "entitled");
  CHECK(iap("required.bundle") == "entitled");
}
TEST_CASE(
    "AssetFile deletion invalidates the installed SKU and rejects escaping "
    "paths") {
  Fixture f;
  auto service =
      f.service([](std::stop_token) { return AssetOwnership{{}, {"ride"}}; });
  CHECK_FALSE(query(service, "DeleteByName", std::string("paid.bundle")).error);
  CHECK(venture::meta::parse_plan(venture::paths::read_json(f.install / "plan.json"))
            .installed_addons.empty());
  CHECK(venture::fs::exists(f.install / "com.example.assets.apk"));
  CHECK(query(service, "StatusByName", std::string("../paid.bundle")).error);
  venture::paths::atomic_write(f.root / "outside.bundle", "12345");
  std::error_code ec;
  venture::fs::create_symlink(f.root / "outside.bundle",
                         f.install / "optional.bundle", ec);
  if (!ec) {
    CHECK(string_field(
              query(service, "StatusByName", std::string("optional.bundle"))
                  .model,
              "DownloadStatus") == "available");
    CHECK(query(service, "DeleteByName", std::string("optional.bundle")).error);
    CHECK(venture::fs::exists(f.root / "outside.bundle"));
  }
}
TEST_CASE(
    "AssetFile downloader refuses gated filenames when the owner has no "
    "purchase") {
  Fixture f;
  venture::test::Server server;
  server.push(R"({"data":[]})");
  std::atomic<int> transferred = 0;
  venture::download::Manager::Services services;
  services.query = store_queries(f);
  services.read = [&](std::string_view url, const venture::http::Fields& headers) {
    auto u = venture::http::parse_url(url);
    return venture::http::get_loopback(server.port, u.path + '?' + u.query, headers);
  };
  services.transfer = [&](std::string, const venture::fs::path&, std::uint64_t,
                          std::stop_token,
                          const venture::http::Progress&) { ++transferred; };
  venture::download::Manager manager(f.root / "queue", [] {}, services);
  manager.enqueue_asset(f.app.app_id, f.root / "games", f.app.binary_id,
                        "444444", "paid.bundle", "fixture", "0123456789ab");
  auto deadline = std::chrono::steady_clock::now() + venture::test::hang_timeout;
  while (manager.busy() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  INFO("gated asset manager busy=", manager.busy(), " jobs=", manager.snapshot().size(),
       " transfers=", transferred.load());
  REQUIRE_FALSE(manager.busy());
  CHECK(transferred == 0);
  CHECK(manager.snapshot("0123456789ab")[0].state ==
        venture::download::State::failed);
}

TEST_CASE("AssetFile returned paths publish new downloads and delete stale OBB views") {
  Fixture f;
  auto service = f.service();
  auto result = query(service, "DownloadByName", std::string("required.bundle"));
  REQUIRE_FALSE(result.error);
  CHECK(venture::paths::read(f.root / "view/required.bundle") == "abc");
  venture::paths::atomic_write(f.install / "optional.bundle", "later");
  result = query(service, "StatusByName", std::string("optional.bundle"));
  REQUIRE_FALSE(result.error);
  CHECK(venture::paths::read(f.root / "view/optional.bundle") == "later");
  result = query(service, "DeleteByName", std::string("optional.bundle"));
  REQUIRE_FALSE(result.error);
  CHECK_FALSE(venture::fs::exists(f.root / "view/optional.bundle"));
  CHECK_FALSE(venture::fs::exists(f.root / "view/plan.json"));
  CHECK_FALSE(venture::fs::exists(f.root / "view/com.example.assets.apk"));
}
TEST_CASE("AssetFile language packs serve the system's regional tag") {
  Fixture f;
  f.plan.files.back().language_tag = "fr";
  venture::paths::atomic_write(f.install / "plan.json", f.plan.json().dump());
  venture::paths::atomic_write(f.install / "fr.bundle", "fr");
  auto s = f.service();
  auto set = [&](std::string tag) {
    RequestArgs args;
    args.values.push_back(std::move(tag));
    return s->request("LanguageSetCurrent", args, {});
  };
  // A pack named for its language ("ko.lang") serves "ko-KR" and the exact tag.
  CHECK_FALSE(set("fr-CA").error);
  CHECK_FALSE(set("FR").error);
  CHECK(set("de-DE").error);
  CHECK(set("f").error);
}
