// The Android Platform loader's dispatcher (src/android_game/platform) on the
// x64 host: exports called through NativeContexts as the native entry slots
// deliver them, against a broker that stands in for the Venture app's
// PlatformService.
#include "android_game/platform/native_platform.h"

#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <stdexcept>
#include <string>

#include "common/android/headset_games.h"
#include "common/file_ops.h"
#include "cpu/native_entry.h"
#include "hle/ovrplatform/abi.h"

namespace fs = std::filesystem;
using namespace venture;
using namespace venture::ovrplatform;

namespace {
constexpr const char* kAppId = "3927222113966481";
constexpr const char* kAccount = "123456abcdef";
constexpr std::array<const char*, 5> kNames{"", "ovr_PlatformInitializeAndroid", "ovr_IsPlatformInitialized",
                                            "ovr_GetLoggedInUserID", "ovr_Venture_NotAnExport"};

Json identity_reply() {
  return Json{{"error", 0}, {"appId", kAppId}, {"package", "com.example.game"}, {"binaryId", "24704002429195146"},
              {"versionCode", 745},    {"versionName", "1.0"},          {"account", kAccount}};
}

// What CredentialBroker::handle answers for a signed-in account.
struct FakeBroker {
  std::atomic<int> calls = 0;
  bool refuse = false;
  Json operator()(const Json& request) {
    ++calls;
    if (refuse) return Json{{"error", static_cast<int>(ErrorCode::identity_mismatch)}};
    const auto op = str(request, "op");
    if (op == "identity") return identity_reply();
    if (op == "credentials")
      return Json{{"version", 1},         {"account", kAccount},
                  {"appId", kAppId},      {"package", "com.example.game"},
                  {"binaryId", "24704002429195146"}, {"versionCode", 745},
                  {"identityError", 0},   {"appToken", "fake-app-token"},
                  {"appUserId", 4242},    {"alias", "player"}};
    if (op == "validate") return Json{{"error", 0}};
    return Json{{"error", static_cast<int>(ErrorCode::unavailable)}};
  }
};

std::uint64_t call(NativePlatform& platform, std::uint64_t id, std::uint64_t x0 = 0) {
  NativeContext context{};
  context.x[16] = id;
  context.x[0] = x0;
  context.kind = native_call_svc(NativeCall::hle_call);
  CHECK(platform.dispatch(context) == VENTURE_NATIVE_EXIT_NONE);
  return context.x[0];
}
}  // namespace

TEST_CASE("the Android Platform loader serves the PC handlers with the broker's credentials") {
  FakeBroker broker;
  int configured = 0;
  NativePlatform platform(kNames, [&] {
    ++configured;
    return NativePlatformConfig{[&](const Json& request) { return broker(request); }, nullptr, {}};
  });
  CHECK(call(platform, 2) == 0);  // not initialized yet
  CHECK(configured == 1);
  CHECK(platform.identity().error == ErrorCode::none);
  CHECK(platform.identity().app.package == "com.example.game");
  CHECK(platform.identity().account == kAccount);
  CHECK(platform.implemented() == 3);

  const std::string app_id = kAppId;
  CHECK(static_cast<std::int32_t>(call(platform, 1, h2g(app_id.c_str()))) ==
        static_cast<std::int32_t>(InitResult::success));
  CHECK(call(platform, 2) == 1);
  CHECK(call(platform, 3) == 4242);
  // Exports the HLE lacks, id 0 and ids past the table return 0.
  CHECK(call(platform, 4, 77) == 0);
  CHECK(call(platform, 0, 77) == 0);
  CHECK(call(platform, kNames.size(), 77) == 0);
  CHECK(configured == 1);
}

TEST_CASE("a game the Venture app refuses gets a platform that fails to initialize") {
  FakeBroker broker;
  broker.refuse = true;
  NativePlatform platform(kNames, [&] {
    return NativePlatformConfig{[&](const Json& request) { return broker(request); }, nullptr, {}};
  });
  const std::string app_id = kAppId;
  CHECK(static_cast<std::int32_t>(call(platform, 1, h2g(app_id.c_str()))) !=
        static_cast<std::int32_t>(InitResult::success));
  CHECK(platform.identity().error == ErrorCode::identity_mismatch);
  CHECK(call(platform, 2) == 0);
  CHECK(call(platform, 3) == 0);
}

TEST_CASE("broker identity: unreachable, refused and accepted") {
  CHECK(broker_identity({}).error == ErrorCode::unavailable);
  CHECK(broker_identity([](const Json&) -> Json { throw std::runtime_error("binder died"); }).error ==
        ErrorCode::unavailable);
  CHECK(broker_identity([](const Json&) { return Json{{"error", -20001}}; }).error == ErrorCode::unauthenticated);
  const auto accepted = broker_identity([](const Json&) { return identity_reply(); });
  CHECK(accepted.error == ErrorCode::none);
  CHECK(accepted.app.app_id == kAppId);
  CHECK(accepted.app.binary_id == "24704002429195146");
  CHECK(accepted.app.version_code == 745);
}

TEST_CASE("the headset games registry records, replaces and finds installed games") {
  const auto root = fs::temp_directory_path() / ("venture-headset-" + random_account_key());
  struct Cleanup {
    fs::path root;
    ~Cleanup() { remove_tree(root.parent_path(), root); }
  } cleanup{root};
  using android::HeadsetGame;
  CHECK(android::load_headset_games(root).empty());
  CHECK_FALSE(android::find_headset_game(root, "com.example.game"));
  const HeadsetGame game{"com.example.game", kAppId, "24704002429195146", 745, "1.0", kAccount};
  android::record_headset_game(root, game);
  android::record_headset_game(root, {"com.example.other", "1", "2", 3, "0.1", kAccount});
  auto updated = game;
  updated.version_code = 746;
  android::record_headset_game(root, updated);
  const auto games = android::load_headset_games(root);
  REQUIRE(games.size() == 2);
  CHECK(android::find_headset_game(root, "com.example.game") == updated);
  CHECK(android::headset_games_file(root) == root / "headset" / "games.json");
}
