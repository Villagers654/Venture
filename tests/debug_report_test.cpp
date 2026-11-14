// SPDX-License-Identifier: GPL-3.0-or-later
#include "launcher/debug_report.h"
#include "launcher/diagnosis.h"
#include "common/process.h"
#include "common/library/library.h"
#include <doctest/doctest.h>
#include <chrono>
#include <format>
#include <fstream>

using namespace venture;
using namespace venture::launcher;

TEST_CASE("debug report: known causes, most specific first") {
  CHECK_FALSE(diagnose("all fine\n").has_value());
  auto xr = diagnose("  1.0 1 E [runtime] Your configured OpenXR runtime is unavailable. Make sure ...\n");
  REQUIRE(xr);
  CHECK(xr->cause.find("No OpenXR runtime") != std::string::npos);
  // A crash after the game could not read its account: the account is the cause.
  // SteamVR saw no headset (Animal Company report): that, not a closing runtime, is the cause.
  auto no_hmd = diagnose("E [runtime] The OpenXR runtime failed to create an instance (XrResult -2). Close ...\n"
                         "[Info] - No connected devices found. Returning best error "
                         "VRInitError_Driver_WirelessHmdNotConnectedAndNoSteam\n");
  REQUIRE(no_hmd);
  CHECK(no_hmd->cause.find("found no headset") != std::string::npos);
  auto vault = diagnose("W [ovrplatform] Meta session load failed stage=vault-session\nE [libc] Fatal signal 11\n");
  REQUIRE(vault);
  CHECK(vault->cause.find("Meta sign-in") != std::string::npos);
  auto lost = diagnose("E [vulkan-fault] Native Vulkan device lost on guest tid 7; ending the app\n");
  REQUIRE(lost);
  CHECK(lost->cause.find("graphics device") != std::string::npos);
  auto reset = diagnose("kernel: amdgpu 0000:03:00.0: amdgpu: GPU reset begin!\n");
  REQUIRE(reset);
  CHECK(reset->cause.find("reset the GPU") != std::string::npos);
  auto trap = diagnose("E [hle] unimplemented libandroid.so!AFoo\nE [libc] Fatal signal 11\n");
  REQUIRE(trap);
  CHECK(trap->cause.find("Android function") != std::string::npos);
  // A crash after the VR session was not created: the session is the cause.
  auto session = diagnose("E [xr] The VR session could not be created: Oculus refused it (XR_ERROR_GRAPHICS_DEVICE_"
                          "INVALID)\nE [libc] Fatal signal 6\n");
  REQUIRE(session);
  CHECK(session->cause.find("VR session") != std::string::npos);
  // An uncaught Java exception closed the app (Pinball FX VR's ProfileInstaller).
  auto java = diagnose("W [vfs] Missing expansion file: the game looked for patch.com.example.obb, which this "
                       "install does not have\nE [AndroidRuntime] FATAL EXCEPTION: pool-3-thread-1: "
                       "java.lang.NoClassDefFoundError: android/content/pm/PackageManager$PackageInfoFlags\n");
  REQUIRE(java);
  CHECK(java->cause.find("uncaught Java exception") != std::string::npos);
  CHECK(java->advice.find("fix in Venture") != std::string::npos);
  // The same on the game's Android main thread (Ballrs, while Unity starts).
  auto main_thread = diagnose("E [java] The game's Android main thread stopped on an uncaught "
                              "java.lang.NoClassDefFoundError: android/hardware/camera2/CameraManager$Availability"
                              "Callback\n");
  REQUIRE(main_thread);
  CHECK(main_thread->cause.find("uncaught Java exception") != std::string::npos);
  // UE4 and Unity probe for optional patch OBBs at every start: the probe alone
  // names no cause (Pinball FX VR quit for another reason and was told to reinstall).
  CHECK_FALSE(diagnose("W [vfs] Missing expansion file: the game looked for patch.com.example.obb, which this "
                       "install does not have\nI [Process] Application requested process exit 0\n").has_value());
  CHECK(diagnose("The Venture launcher needs a Wayland session, and no Wayland display was reachable")->cause.find(
            "Wayland") != std::string::npos);
  CHECK(error_line("Something FAILED here"));
  CHECK(error_line("connection timed out"));
  CHECK_FALSE(error_line("errorless profile, xidle"));
  CHECK(noisy_line("Failed to parse bindings for ViveController"));
}

TEST_CASE("debug report: a failed run, its times, cause and next steps come first") {
  const auto data = fs::temp_directory_path() / ("venture-report-run-" + std::to_string(current_process_id()));
  fs::remove_all(data);
  fs::create_directories(data / "logs");
  const auto start = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()) - std::chrono::minutes(2);
  std::ofstream(data / "logs" / "runtime-com.example.game.log", std::ios::binary)
      << std::format("  1.000 1 I [launch] Process started at {:%FT%TZ}\n"
                     "  1.100 2 I [launch] Process started at {:%FT%TZ}\n"
                     "  2.000 2 W [ovrplatform] Meta session load failed stage=vault-session\n"
                     "  3.000 2 E [libc] Fatal signal 11 (SIGSEGV), code 1 in tid 2\n"
                     "  3.100 1 I [launch] Exit code: 139\n",
                     start, start + std::chrono::seconds(1));
  auto file = write_debug_report({.data = data, .version = "9.9.9", .package = "com.example.game",
                                  .game = "Example", .private_text = {"PrivateAlias"}});
  std::ifstream in(file, std::ios::binary);
  std::string text{std::istreambuf_iterator<char>(in), {}};
  CHECK(text.find("Game: Example (com.example.game)") != std::string::npos);
  CHECK(text.find("Last run: ") != std::string::npos);
  CHECK(text.find("start estimated") == std::string::npos);
  CHECK(text.find("Result: The runtime stopped unexpectedly (exit code 139).") != std::string::npos);
  CHECK(text.find("=== Likely cause ===\nThe game could not read your Meta sign-in.") != std::string::npos);
  CHECK(text.find("=== Next steps ===\n- Make sure your desktop keyring") != std::string::npos);
  CHECK(text.find("=== Errors logged during the last run") != std::string::npos);
  // The summary comes before the system details.
  CHECK(text.find("=== Likely cause ===") < text.find("=== System ==="));
  in.close();

  // launcher.log also holds earlier runs: an earlier run's runtime failure is not
  // this run's cause when the run's own log names one (Ballrs).
  std::ofstream(data / "logs" / "launcher.log", std::ios::binary)
      << "  0.500 9 W [launcher] Example stopped: The OpenXR runtime failed to create an instance (XrResult -2).\n";
  std::ofstream(data / "logs" / "runtime-com.example.game.log", std::ios::binary)
      << std::format("  1.000 1 I [launch] Process started at {:%FT%TZ}\n"
                     "  3.000 2 E [libc] Fatal signal 11 (SIGSEGV), code 1 in tid 2\n"
                     "  3.100 1 I [launch] Exit code: 139\n",
                     start);
  file = write_debug_report({.data = data, .version = "9.9.9", .package = "com.example.game"});
  in.open(file, std::ios::binary);
  text.assign(std::istreambuf_iterator<char>(in), {});
  CHECK(text.find("=== Likely cause ===\nThe game crashed.") != std::string::npos);
  in.close();
  fs::remove(data / "logs" / "launcher.log");

  // A run stopped from Venture is not a failure.
  std::ofstream(data / "logs" / "runtime-com.example.game.log", std::ios::binary)
      << std::format("  1.000 1 I [launch] Process started at {:%FT%TZ}\n  9.000 1 I [launch] Exit code: 0 (stop "
                     "requested)\n",
                     start);
  file = write_debug_report({.data = data, .version = "9.9.9", .package = "com.example.game"});
  in.open(file, std::ios::binary);
  text.assign(std::istreambuf_iterator<char>(in), {});
  CHECK(text.find("Result: stopped from Venture (exit code 0)") != std::string::npos);
  CHECK(text.find("The last run did not fail.") != std::string::npos);
  in.close();

  // A game that got no Meta user waits for one (Pinball FX VR's black screen): the report says to sign in.
  std::ofstream(data / "logs" / "runtime-com.example.game.log", std::ios::binary)
      << std::format("  1.000 1 I [launch] Process started at {:%FT%TZ}\n"
                     "  2.000 1 W [accounts] Application credentials received without a user: error -20001 "
                     "(-20001 means no Meta account is signed in for this game)\n"
                     "  9.000 1 I [launch] Exit code: 0 (stop requested)\n",
                     start);
  file = write_debug_report({.data = data, .version = "9.9.9", .package = "com.example.game"});
  in.open(file, std::ios::binary);
  text.assign(std::istreambuf_iterator<char>(in), {});
  CHECK(text.find("=== Likely cause ===\nThe game ran without a Meta user") != std::string::npos);
  CHECK(text.find("- Sign in with Meta in Venture") != std::string::npos);
  in.close();

  // A game that closed on its failed ownership check: the sign-in is the cause, not another
  // game's crash in launcher.log (I Am Cat was told Table Troopers' Java exception).
  std::ofstream(data / "logs" / "launcher.log", std::ios::binary)
      << "  0.500 9 W [launcher] com.other.game stopped: The game's Android main thread stopped on an uncaught "
         "java.lang.NoClassDefFoundError: android/hardware/camera2/CameraManager$AvailabilityCallback\n";
  std::ofstream(data / "logs" / "runtime-com.example.game.log", std::ios::binary)
      << std::format("  1.000 1 I [launch] Process started at {:%FT%TZ}\n"
                     "  2.000 1 W [accounts] Application credentials received without a user: error -20001 "
                     "(-20001 means no Meta account is signed in for this game)\n"
                     "  2.100 1 W [ovrplatform] ovr_Entitlement_GetIsViewerEntitled request=1 type=0x1 error=-20001\n"
                     "  4.000 1 I [launch] Exit code: 0\n",
                     start);
  file = write_debug_report({.data = data, .version = "9.9.9", .package = "com.example.game"});
  in.open(file, std::ios::binary);
  text.assign(std::istreambuf_iterator<char>(in), {});
  CHECK(text.find("Result: closed: it needs a signed-in Meta account that owns it") != std::string::npos);
  CHECK(text.find("=== Likely cause ===\nNo Meta account was signed in for the game") != std::string::npos);
  CHECK(text.find("- Sign in with Meta in Venture") != std::string::npos);
  in.close();

  // Another game's stop line in launcher.log names no cause for this run.
  std::ofstream(data / "logs" / "runtime-com.example.game.log", std::ios::binary)
      << std::format("  1.000 1 I [launch] Process started at {:%FT%TZ}\n  3.100 1 I [launch] Exit code: 139\n",
                     start);
  file = write_debug_report({.data = data, .version = "9.9.9", .package = "com.example.game"});
  in.open(file, std::ios::binary);
  text.assign(std::istreambuf_iterator<char>(in), {});
  CHECK(text.find("uncaught Java exception") == std::string::npos);
  in.close();
  fs::remove_all(data);
}

TEST_CASE("debug report: the game's overrides, effective settings and launch environment") {
  const auto data = fs::temp_directory_path() / ("venture-report-settings-" + std::to_string(current_process_id()));
  fs::remove_all(data);
  fs::create_directories(data / "logs");
  std::ofstream(data / "settings.json")
      << R"({"textureCompression": 1, "keepTranslatedCode": false, "vulkanLayers": ["VK_LAYER_reshade*"],
             "display": {"mirror": "full"},
             "gameDisplay": {"1234": {"resolution": "quest3", "pcAudio": "on"},
                             "5678": {"textureCompression": "auto", "textureCompressionThreshold": 70}},
             "controls": {"games": {"1234": {}}}})";
  auto report = [&](std::string app_id) {
    auto file = write_debug_report({.data = data, .version = "9.9.9", .package = "com.example.game",
                                    .app_id = std::move(app_id)});
    std::ifstream in(file, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(in), {}};
  };
  auto text = report("1234");
  CHECK(text.find("=== Settings for this game ===") != std::string::npos);
  CHECK(text.find("Game overrides:\n  resolution: 1680x1760\n  PC audio: on\n  controls: ") != std::string::npos);
  CHECK(text.find("  resolution: Quest 3 (1680x1760) (game override)") != std::string::npos);
  CHECK(text.find("  frame rate: the host runtime's rate (global default)") != std::string::npos);
  CHECK(text.find("  mirror: full (global setting), eye left (global default)") != std::string::npos);
  CHECK(text.find("  PC audio: on (game override)") != std::string::npos);
  CHECK(text.find("  texture compression: Automatic (global setting), threshold 50% (global default)") !=
        std::string::npos);
  CHECK(text.find("  controls: the game's own") != std::string::npos);
  CHECK(text.find("  keep translated code: off (global setting)") != std::string::npos);
  CHECK(text.find("  overlay blocking: on, except the kept layers VK_LAYER_reshade* (global setting)") !=
        std::string::npos);
  // The environment is the one the launch builds.
  for (const auto &[key, value] : library::launch_environment(library::Settings::load(data), "1234"))
    CHECK(text.find(std::format("  {}={}\n", key, value)) != std::string::npos);
  CHECK(text.find("  VENTURE_JIT_PTC=0\n") != std::string::npos);
  CHECK(text.find("  VENTURE_TEXTURE_COMPRESSION=auto\n") != std::string::npos);

  text = report("5678");
  CHECK(text.find("Game overrides:\n  texture compression: auto\n  texture compression threshold: 70%\n") !=
        std::string::npos);
  CHECK(text.find("  texture compression: Automatic (game override), threshold 70% (game override)") !=
        std::string::npos);
  CHECK(text.find("  controls: the global controls") != std::string::npos);

  text = report("");
  CHECK(text.find("Game overrides: (game not identified; the global settings apply)") != std::string::npos);
  fs::remove_all(data);
}

TEST_CASE("debug report: the session timeline comes from the whole runtime log") {
  const auto data = fs::temp_directory_path() / ("venture-report-timeline-" + std::to_string(current_process_id()));
  fs::remove_all(data);
  fs::create_directories(data / "logs");
  const auto log = data / "logs" / "runtime-com.example.game.log";
  const auto report = [&] {
    auto file = write_debug_report({.data = data, .version = "9.9.9", .package = "com.example.game"});
    std::ifstream in(file, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(in), {}};
  };
  {
    std::ofstream out(log, std::ios::binary);
    out << "  1.000 1 I [launch] Process started\n";
    // The log's middle, which the runtime log section cuts, holds the timeline.
    for (int minute = 1; minute <= 200; ++minute) {
      out << std::format("{:7}.000 9 I [timeline] t={}m fps={} worst_frame_ms=9.0 long_frames=0\n", minute * 60,
                         minute, minute < 30 ? 120 : 90);
      for (int i = 0; i < 400; ++i) out << std::format("{:7}.500 3 I [game] filler line {}\n", minute * 60, i);
    }
  }
  auto text = report();
  CHECK(text.find("=== Session timeline (200 minutes, 120 shown) ===") != std::string::npos);
  CHECK(text.find("[timeline] t=1m fps=120") != std::string::npos);
  CHECK(text.find("[timeline] t=200m fps=90") != std::string::npos);
  auto section = text.substr(text.find("=== Session timeline"));
  section = section.substr(0, section.find("\n===", 4));
  std::size_t lines = 0;
  for (std::size_t at = 0; (at = section.find(" [timeline] ", at)) != std::string::npos; ++at) ++lines;
  CHECK(lines == 120);

  std::ofstream(log, std::ios::binary) << "  1.000 1 I [launch] Process started\n"
                                          "     60.000 9 I [timeline] t=1m fps=72.0\n";
  text = report();
  CHECK(text.find("=== Session timeline (1 minutes) ===\n     60.000 9 I [timeline] t=1m fps=72.0\n") !=
        std::string::npos);
  fs::remove_all(data);
}
