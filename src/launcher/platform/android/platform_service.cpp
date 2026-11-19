// SPDX-License-Identifier: GPL-3.0-or-later
// The core of the Venture app's PlatformService: answers the Platform broker
// requests of the games Venture installed on this headset, as the PC launcher
// answers its game's pipe (CredentialBroker), with the account each game was
// installed for. PlatformService has already checked that the calling package
// is signed with the user's game key; this checks that Venture installed it.
#include <jni.h>

#include <map>
#include <memory>
#include <mutex>

#include "common/android/headset_games.h"
#include "common/json.h"
#include "common/log.h"
#include "common/paths.h"
#include "common/platform/android/jni.h"
#include "hle/ovrplatform/broker.h"
#include "launcher/headset_play.h"

using namespace venture;

namespace {
struct Served {
  android::HeadsetGame game;
  std::shared_ptr<ovrplatform::CredentialBroker> broker;
};
std::mutex g_mutex;
std::map<std::string, Served, std::less<>> g_served;

Json answer(const android::HeadsetGame& game, const Json& request) {
  if (str(request, "op") == "identity")
    return Json{{"error", 0},
                {"appId", game.app_id},
                {"package", game.package},
                {"binaryId", game.binary_id},
                {"versionCode", game.version_code},
                {"versionName", game.version_name},
                {"account", game.account}};
  // Asset file downloads go to the launcher's download manager, which keeps
  // the game's files; credentials never reach the game.
  if (str(request, "op").starts_with("asset.")) {
    if (auto reply = launcher::headset::asset_request(game.package, request)) return *reply;
    return Json{{"ok", false}};
  }
  std::shared_ptr<ovrplatform::CredentialBroker> broker;
  {
    std::lock_guard lock(g_mutex);
    auto& served = g_served[game.package];
    if (!served.broker || !(served.game == game)) {
      // A reinstall for another account or version starts over.
      served.game = game;
      served.broker = std::make_shared<ovrplatform::CredentialBroker>(
          ovrplatform::AppIdentity{game.app_id, game.package, game.binary_id, game.version_code, game.version_name},
          game.account, ovrplatform::runtime_secret_store(game.account), ovrplatform::meta_http_transport());
      if (auto error = served.broker->prepare()) {
        served.broker.reset();
        return Json{{"error", static_cast<int>(error.code)}};
      }
    }
    broker = served.broker;
  }
  return broker->handle(request);
}
}  // namespace

extern "C" JNIEXPORT jstring JNICALL Java_org_venture_1emu_venture_Native_platformRequest(JNIEnv* env, jclass,
                                                                                          jstring package,
                                                                                          jstring request) {
  const auto name = android::utf8(env, package);
  try {
    const auto game = android::find_headset_game(paths::data_dir(), name);
    if (!game) return nullptr;
    return android::java_string(env, answer(*game, parse_json(android::utf8(env, request))).dump());
  } catch (const std::exception& e) {
    log::warn("platform", "the Platform request of {} failed: {}", name, e.what());
    return android::java_string(
        env, Json{{"error", static_cast<int>(ovrplatform::ErrorCode::unavailable)}}.dump());
  }
}
