// Development tool: adapts a game APK for a described headset on the PC, with
// the code Play on an Android headset runs on the headset
// (launcher/platform/android/headset_play.cpp): the plan, then adapt_game and
// rewrite_apk. The result is unsigned. tools/android/headset_play_test.py
// --direct uses it where an emulator image runs a 32-bit game but not the
// Venture app itself (the API 30 image's ARM translation).
//
//   venture_adapt_game GAME.apk VENTURE.apk OUT.apk --abis A,B,... --api N
//                      [--xr-minor N] [--ext NAME:VERSION ...] [--work DIR]
//
// Prints the plan's ABI; fails with the plan's first blocker.
#include "common/android/apk_rewrite.h"
#include "common/android/game_adapter.h"
#include "common/android/native_host.h"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>

namespace fs = std::filesystem;
namespace nh = venture::android::native_host;

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fputs("usage: venture_adapt_game GAME.apk VENTURE.apk OUT.apk --abis A,B --api N [--xr-minor N] "
               "[--ext NAME:VERSION ...] [--work DIR]\n",
               stderr);
    return 2;
  }
  try {
    const fs::path game = argv[1], venture = argv[2], out = argv[3];
    nh::HostProfile host;
    fs::path work = out;
    work += ".work";
    for (int i = 4; i + 1 < argc; i += 2) {
      const std::string_view option = argv[i];
      const std::string value = argv[i + 1];
      if (option == "--abis") {
        for (std::size_t start = 0; start <= value.size();) {
          const auto end = std::min(value.find(',', start), value.size());
          if (end > start) host.abis.push_back(value.substr(start, end - start));
          start = end + 1;
        }
      } else if (option == "--api") {
        host.android_api = std::stoi(value);
      } else if (option == "--xr-minor") {
        host.xr_api_minor = static_cast<std::uint32_t>(std::stoul(value));
      } else if (option == "--ext") {
        const auto colon = value.find(':');
        host.xr_extensions[value.substr(0, colon)] =
            colon == std::string::npos ? 1 : static_cast<std::uint32_t>(std::stoul(value.substr(colon + 1)));
      } else if (option == "--work") {
        work = value;
      } else {
        std::fprintf(stderr, "venture_adapt_game: unknown option %s\n", argv[i]);
        return 2;
      }
    }
    const auto plan = nh::plan(nh::profile(game), host);
    if (!plan.blockers.empty()) {
      std::fprintf(stderr, "venture_adapt_game: %s\n", plan.blockers.front().c_str());
      return 1;
    }
    if (plan.execution != nh::Execution::native) {
      std::fputs("venture_adapt_game: the game does not run natively on this host\n", stderr);
      return 1;
    }
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(work);
    const auto edit = venture::android::adapt_game(game, plan, venture, work);
    venture::android::rewrite_apk(game, edit, out);
    fs::remove_all(work, ec);
    std::printf("abi=%s\n", plan.abi.c_str());
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "venture_adapt_game: %s\n", e.what());
    return 1;
  }
}
