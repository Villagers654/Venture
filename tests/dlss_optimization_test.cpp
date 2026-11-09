#include "launcher/dlss_optimization.h"
#include "common/paths.h"
#include <doctest/doctest.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>

using namespace venture;
using namespace venture::launcher;
namespace {
struct Folder {
  fs::path root = fs::temp_directory_path() / ("venture-dlss-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()));
  Folder() { fs::create_directories(root); }
  ~Folder() { std::error_code error; fs::remove_all(root, error); }
};
const std::string* find(const Environment& environment, std::string_view key) {
  for (const auto& [name, value] : environment)
    if (name == key) return &value;
  return nullptr;
}
std::string read(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}
void touch(const fs::path& path) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << "x";
}
}  // namespace

TEST_CASE("DLSS VR Optimization is off for a game without a profile and adds nothing at launch") {
  Folder data;
  const auto settings = DlssOptimization::load(data.root);
  CHECK(settings.games.empty());
  CHECK_FALSE(settings.profile("com.example.game").enabled);
  Environment environment;
  CHECK(apply_dlss_optimization(settings, "com.example.game", data.root / "venture-runtime.exe", environment).empty());
  CHECK(environment.empty());
}

TEST_CASE("DLSS VR Optimization profiles survive a save and clamp out-of-range values") {
  Folder data;
  DlssOptimization settings;
  settings.cheeky_dir = data.root / "cheeky";
  settings.nvidia_dir = data.root / "nvidia";
  auto& p = settings.games["com.example.game"];
  p.enabled = true;
  p.source_percent = 72;
  p.eye_tracking = false;
  apply_sr_defaults(p);
  apply_nr_defaults(p);
  p.set(nr_pass_key(1, "Intensity"), 0.5f);
  p.set("NrCacheMaxInterval", 3.4f);  // an integer setting
  p.set("NotACheekyKey", 1.f);        // unknown keys are not kept
  settings.save(data.root);

  auto loaded = DlssOptimization::load(data.root);
  CHECK(loaded.cheeky_dir == settings.cheeky_dir);
  CHECK(loaded.nvidia_dir == settings.nvidia_dir);
  const auto q = loaded.profile("com.example.game");
  CHECK(q.enabled);
  CHECK(q.source_percent == 80);  // apply_sr_defaults
  CHECK_FALSE(q.eye_tracking);
  CHECK(q.value("NrIntensity") == doctest::Approx(0.5f));
  CHECK(q.value("NrCacheMaxInterval") == 3);
  CHECK(q.value("NrPreset") == 2);  // NR model B
  CHECK_FALSE(q.values.contains("NotACheekyKey"));

  auto json = paths::read_json(data.root / "dlss-optimization.json");
  json["games"]["com.example.game"]["sourcePercent"] = 5;
  json["games"]["com.example.game"]["srWidth"] = 400;
  paths::atomic_write(data.root / "dlss-optimization.json", json.dump());
  loaded = DlssOptimization::load(data.root);
  CHECK(loaded.profile("com.example.game").source_percent == 30);
  CHECK(loaded.profile("com.example.game").sr_width == 100);
}

TEST_CASE("DLSS VR Optimization passes the render scale and DLSS to the runtime and names missing files") {
  Folder folder;
  const auto runtime = folder.root / "runtime" / "venture-runtime.exe";
  fs::create_directories(runtime.parent_path());
  DlssOptimization settings;
  auto& p = settings.games["com.example.game"];
  p.enabled = true;
  p.cheeky = false;
  p.source_percent = 65;
  Environment environment;
  const auto problems = apply_dlss_optimization(settings, "com.example.game", runtime, environment);
  REQUIRE(find(environment, "VENTURE_EXPERIMENTAL_RENDER_SCALE"));
  CHECK(*find(environment, "VENTURE_EXPERIMENTAL_RENDER_SCALE") == "0.65");
  REQUIRE(find(environment, "VENTURE_EXPERIMENTAL_DLSS"));
  CHECK(*find(environment, "VENTURE_EXPERIMENTAL_DLSS") == "1");
  CHECK_FALSE(find(environment, "VENTURE_DLSS_PATH"));
  CHECK_FALSE(find(environment, "VK_ADD_IMPLICIT_LAYER_PATH"));
  const auto names = [&](std::string_view file) {
    return std::ranges::any_of(problems, [&](const std::string& problem) { return problem.contains(file); });
  };
  // A bridge beside the test executable counts as the build's own.
  CHECK(names("venture_ngx.dll") != fs::is_regular_file(paths::executable().parent_path() / "venture_ngx.dll"));
  CHECK(names("nvngx_dlss.dll"));

  // The NVIDIA folder supplies the DLSS model; the bridge beside the runtime counts.
  settings.nvidia_dir = folder.root / "nvidia";
  touch(settings.nvidia_dir / "nvngx_dlss.dll");
  touch(runtime.parent_path() / "venture_ngx.dll");
  environment.clear();
  CHECK(apply_dlss_optimization(settings, "com.example.game", runtime, environment).empty());
  REQUIRE(find(environment, "VENTURE_DLSS_PATH"));
  CHECK(*find(environment, "VENTURE_DLSS_PATH") == paths::utf8(settings.nvidia_dir));
}

#ifdef _WIN32
TEST_CASE("DLSS VR Optimization deploys Cheeky beside the runtime and merges its settings") {
  Folder folder;
  const auto runtime = folder.root / "runtime" / "venture-runtime.exe";
  touch(runtime.parent_path() / "venture_ngx.dll");
  DlssOptimization settings;
  settings.cheeky_dir = folder.root / "cheeky";
  settings.nvidia_dir = folder.root / "nvidia";
  for (const auto& file : {fs::path("CheekyVulkanLayer.dll"), fs::path("CheekyFoveatedDLSS/CheekyFoveatedDLSSHost.dll"),
                           fs::path("CheekyFoveatedDLSS/CheekyFoveatedDLSSRuntime.dll"),
                           fs::path("CheekyFoveatedDLSS/Vulkan/Cheeky.json")})
    touch(settings.cheeky_dir / file);
  touch(settings.nvidia_dir / "nvngx_dlss.dll");
  touch(settings.nvidia_dir / "nvngx_dlssnr.dll");
  // A setting Cheeky has that the tab does not edit stays as it was.
  const auto deployed = runtime.parent_path() / "CheekyFoveatedDLSS";
  fs::create_directories(deployed);
  std::ofstream(deployed / "CheekyFoveatedDLSS.ini", std::ios::binary)
      << "[CheekyFoveatedDLSS]\r\nSchemaVersion=1\r\nGazeSmoothingMs=50\r\nWidth=0.10\r\n[Other]\r\nKey=1\r\n";
  auto& p = settings.games["com.example.game"];
  p.enabled = true;
  p.neural_rendering = true;
  p.nr_thumbstick = true;
  p.sr_width = 45;

  Environment environment;
  CHECK(apply_dlss_optimization(settings, "com.example.game", runtime, environment).empty());
  CHECK(fs::is_regular_file(runtime.parent_path() / "CheekyVulkanLayer.dll"));
  CHECK(fs::is_regular_file(deployed / "CheekyFoveatedDLSSHost.dll"));
  CHECK(fs::is_regular_file(deployed / "nvngx_dlssnr.dll"));
  REQUIRE(find(environment, "VK_ADD_IMPLICIT_LAYER_PATH"));
  CHECK(*find(environment, "VK_ADD_IMPLICIT_LAYER_PATH") == paths::utf8(deployed / "Vulkan"));
  CHECK(find(environment, "VENTURE_NR_THUMBSTICK"));
  CHECK(find(environment, "VENTURE_CHEEKY_NR"));
  const auto ini = read(deployed / "CheekyFoveatedDLSS.ini");
  CHECK(ini.find("GazeSmoothingMs=50\r\n") != std::string::npos);
  CHECK(ini.find("Width=0.45\r\n") != std::string::npos);
  CHECK(ini.find("NrEnabled=1\r\n") != std::string::npos);
  // Cheeky's Vulkan NR runs one pass.
  CHECK(ini.find("NrSecondPass=0\r\n") != std::string::npos);
  // New keys go into Cheeky's section, before the next one.
  CHECK(ini.find("NrEnabled=1") < ini.find("[Other]"));
  CHECK(ini.find("[Other]\r\nKey=1\r\n") != std::string::npos);

  // Apply in game: a command for the running runtime, answered in a status file.
  std::string error;
  const auto request = apply_in_game(settings, "com.example.game", runtime, error);
  REQUIRE_FALSE(request.empty());
  CHECK(error.empty());
  const auto command = read(deployed / "venture-live-command.txt");
  CHECK(command.starts_with("1\n" + request + "\nset\n"));
  CHECK(command.find("\nNrEnabled=true\n") != std::string::npos);
  CHECK_FALSE(in_game_status(runtime, request));
  std::ofstream(deployed / "venture-live-status.txt", std::ios::binary) << request << " 1\n";
  CHECK(in_game_status(runtime, request) == true);
  CHECK_FALSE(in_game_status(runtime, "another request"));
}
#endif
