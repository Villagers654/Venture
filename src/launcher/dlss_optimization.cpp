#include "launcher/dlss_optimization.h"
#include "common/log.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
#include <sstream>

namespace venture::launcher {
namespace {
constexpr const char* kFile = "dlss-optimization.json";

std::vector<CheekyOption> make_options() {
  std::vector<CheekyOption> o{
      // SR: source quality and advanced.
      {"PeripheralSrEnabled", 0, true, true},
      {"PeripheralFsr1Enabled", 0, true, true},
      {"PeripheralSrScale", 1, false},
      {"PeripheralFsr1Scale", 0.3f, false},
      {"CenterSupersampling", 1, false},
      {"Roundness", 0, false},
      {"TransitionWidth", 0.04f, false},
      // NR: passes, cache and advanced.
      {"NrAdaptiveCache", 0, true, true},
      {"NrSecondPass", 0, true, true},
      {"NrThirdPass", 0, true, true},
      {"NrEditCache", 0, true, true},
      {"NrGpuBudgetMs", 0, false},
      {"NrCacheMaxInterval", 2, true},
      {"NrTransitionWidth", 0.08f, false},
      {"NrUseSrFoveation", 0, true, true},
      {"NrRoundness", 0, false},
  };
  static const std::vector<std::string> pass_keys = [] {
    std::vector<std::string> keys;
    for (int pass = 1; pass <= 3; ++pass)
      for (const char* name : {"WorkingScale", "Intensity", "LocalToneStrength", "LocalStructureStrength",
                               "SkinStructureStrength", "Preset", "Style"})
        keys.push_back(nr_pass_key(pass, name));
    return keys;
  }();
  for (const auto& key : pass_keys) {
    const bool choice = key.ends_with("Preset") || key.ends_with("Style");
    o.push_back({key.c_str(), choice ? 0.f : 1.f, choice});
  }
  return o;
}

Json to_json(const DlssGameProfile& p) {
  return {{"enabled", p.enabled},          {"nrThumbstick", p.nr_thumbstick}, {"sourcePercent", p.source_percent},
          {"cheeky", p.cheeky},            {"eyeTracking", p.eye_tracking},
          {"superResolution", p.super_resolution}, {"srDebug", p.sr_debug},
          {"srWidth", p.sr_width},         {"srHeight", p.sr_height},
          {"neuralRendering", p.neural_rendering}, {"nrDebug", p.nr_debug},
          {"nrFoveated", p.nr_foveated},   {"nrWidth", p.nr_width},
          {"nrHeight", p.nr_height},       {"cheekyValues", Json(p.values)}};
}
template <class T> void read(const Json& j, const char* key, T& value) {
  const auto it = j.find(key);
  if (it == j.end()) return;
  if constexpr (std::is_same_v<T, bool>) {
    if (it->is_boolean()) value = it->get<bool>();
  } else {
    if (it->is_number()) value = it->get<T>();
  }
}
DlssGameProfile from_json(const Json& j) {
  DlssGameProfile p;
  if (!j.is_object()) return p;
  read(j, "enabled", p.enabled);
  read(j, "nrThumbstick", p.nr_thumbstick);
  read(j, "sourcePercent", p.source_percent);
  read(j, "cheeky", p.cheeky);
  read(j, "eyeTracking", p.eye_tracking);
  read(j, "superResolution", p.super_resolution);
  read(j, "srDebug", p.sr_debug);
  read(j, "srWidth", p.sr_width);
  read(j, "srHeight", p.sr_height);
  read(j, "neuralRendering", p.neural_rendering);
  read(j, "nrDebug", p.nr_debug);
  read(j, "nrFoveated", p.nr_foveated);
  read(j, "nrWidth", p.nr_width);
  read(j, "nrHeight", p.nr_height);
  // "cheekyValues": Cheeky INI keys (DlssGameProfile::values). "cheeky" is the
  // layer switch.
  if (const auto it = j.find("cheekyValues"); it != j.end() && it->is_object())
    for (const auto& [key, value] : it->items())
      if (value.is_number()) p.set(key, value.get<float>());
  // Before the full setting list: NR intensity had its own field.
  if (const auto it = j.find("nrIntensity"); it != j.end() && it->is_number() && !p.values.contains("NrIntensity"))
    p.set("NrIntensity", it->get<float>());
  p.source_percent = std::clamp(p.source_percent, 30, 95);
  p.sr_width = std::clamp(p.sr_width, 10, 100);
  p.sr_height = std::clamp(p.sr_height, 10, 100);
  p.nr_width = std::clamp(p.nr_width, 10, 100);
  p.nr_height = std::clamp(p.nr_height, 10, 100);
  return p;
}

bool file(const fs::path& path) {
  std::error_code ec;
  return !path.empty() && fs::is_regular_file(path, ec);
}

// Sets keys in the [CheekyFoveatedDLSS] section and keeps every other line.
void merge_ini(const fs::path& path, const std::vector<std::pair<std::string, std::string>>& values) {
  std::vector<std::string> lines;
  {
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      lines.push_back(std::move(line));
    }
  }
  if (lines.empty()) lines = {"[CheekyFoveatedDLSS]", "SchemaVersion=1"};
  auto section_end = lines.size();
  bool in_section = false;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (!lines[i].empty() && lines[i].front() == '[') {
      if (in_section) {
        section_end = i;
        break;
      }
      in_section = lines[i] == "[CheekyFoveatedDLSS]";
    }
  }
  for (const auto& [key, value] : values) {
    const auto prefix = key + "=";
    auto it = std::ranges::find_if(lines, [&](const std::string& line) { return line.starts_with(prefix); });
    if (it != lines.end()) {
      *it = prefix + value;
    } else {
      lines.insert(lines.begin() + std::ptrdiff_t(std::min(section_end, lines.size())), prefix + value);
      ++section_end;
    }
  }
  std::string text;
  for (const auto& line : lines) text += line + "\r\n";
  paths::atomic_write(path, text);
}
std::string fraction(int percent) { return std::format("{:.2f}", percent / 100.0); }
// venture_ngx.dll ships beside the launcher (build output, install folder); the
// runtime loads it from its own folder.
fs::path bridge_source(const fs::path& runtime) {
  const auto beside = runtime.parent_path() / "venture_ngx.dll";
  if (file(beside)) return beside;
  const auto launcher = paths::executable().parent_path() / "venture_ngx.dll";
  return file(launcher) ? launcher : fs::path{};
}
}  // namespace

const std::vector<CheekyOption>& cheeky_options() {
  static const auto options = make_options();
  return options;
}
std::string nr_pass_key(int pass, std::string_view name) {
  return pass <= 1 ? std::format("Nr{}", name) : std::format("NrPass{}{}", pass, name);
}
float DlssGameProfile::value(std::string_view key) const {
  if (const auto it = values.find(key); it != values.end()) return it->second;
  for (const auto& option : cheeky_options())
    if (key == option.key) return option.initial;
  return 0;
}
void DlssGameProfile::set(std::string_view key, float value) {
  for (const auto& option : cheeky_options())
    if (key == option.key) {
      values[std::string(key)] = option.integer ? std::round(value) : value;
      return;
    }
}

// The game's Cheeky settings as INI or live-command values: the INI writes
// 0 / 1, a live "set" command true / false.
std::vector<std::pair<std::string, std::string>> cheeky_values(const DlssGameProfile& p, bool live) {
  const auto flag = [&](bool on) { return std::string(live ? (on ? "true" : "false") : (on ? "1" : "0")); };
  std::vector<std::pair<std::string, std::string>> values{
      {"Enabled", flag(p.super_resolution)},  {"CenterMode", p.eye_tracking ? "1" : "0"},
      {"AlignmentBorder", flag(p.sr_debug)},   {"Width", fraction(p.sr_width)},
      {"Height", fraction(p.sr_height)},       {"NrEnabled", flag(p.neural_rendering)},
      {"NrAlignmentBorder", flag(p.nr_debug)}, {"NrFoveated", flag(p.nr_foveated)},
      {"NrWidth", fraction(p.nr_width)},       {"NrHeight", fraction(p.nr_height)}};
  for (const auto& option : cheeky_options()) {
    // Cheeky's Vulkan NR runs one pass: passes 2 and 3 stay off (greyed in the tab).
    const std::string_view key = option.key;
    const float v = key == "NrSecondPass" || key == "NrThirdPass" ? 0.f : p.value(option.key);
    values.emplace_back(option.key, option.toggle    ? flag(v != 0)
                                    : option.integer ? std::format("{}", int(std::lround(v)))
                                                     : std::format("{:.3f}", v));
  }
  return values;
}

DlssOptimization DlssOptimization::load(const fs::path& data) {
  DlssOptimization out;
  const auto path = data / kFile;
  if (!file(path)) return out;
  try {
    const auto j = paths::read_json(path);
    out.cheeky_dir = paths::from_utf8(str(j, "cheekyDirectory"));
    out.nvidia_dir = paths::from_utf8(str(j, "nvidiaDirectory"));
    if (const auto it = j.find("games"); it != j.end() && it->is_object())
      for (const auto& [package, value] : it->items()) out.games[package] = from_json(value);
  } catch (const std::exception& e) {
    log::warn("dlss", "{} could not be read: {}", kFile, e.what());
  }
  return out;
}

void DlssOptimization::save(const fs::path& data) const {
  Json games = Json::object();
  for (const auto& [package, profile] : this->games) games[package] = to_json(profile);
  const Json j{{"cheekyDirectory", paths::utf8(cheeky_dir)},
               {"nvidiaDirectory", paths::utf8(nvidia_dir)},
               {"games", games}};
  paths::atomic_write(data / kFile, j.dump(2));
}

void apply_sr_defaults(DlssGameProfile& p) {
  p.sr_debug = true;
  p.sr_width = p.sr_height = 45;
  p.source_percent = 80;
  p.set("PeripheralSrEnabled", 0.f);
  p.set("PeripheralFsr1Enabled", 1.f);
  p.set("PeripheralFsr1Scale", 0.3f);
  p.set("CenterSupersampling", 1.f);
}

void apply_nr_defaults(DlssGameProfile& p) {
  p.nr_debug = true;
  p.nr_foveated = true;
  p.nr_width = p.nr_height = 30;
  p.set("NrAdaptiveCache", 0.f);
  p.set("NrSecondPass", 0.f);
  p.set("NrThirdPass", 0.f);
  p.set(nr_pass_key(1, "WorkingScale"), 0.8f);
  for (const char* name : {"Intensity", "LocalToneStrength", "LocalStructureStrength", "SkinStructureStrength"})
    p.set(nr_pass_key(1, name), 1.f);
  p.set(nr_pass_key(1, "Preset"), 2.f);  // NR model B
  p.set(nr_pass_key(1, "Style"), 0.f);
}

DlssGameProfile DlssOptimization::profile(std::string_view package) const {
  const auto it = games.find(package);
  return it == games.end() ? DlssGameProfile{} : it->second;
}

DlssReadiness dlss_readiness(const DlssOptimization& settings, const fs::path& runtime) {
  const auto beside = runtime.parent_path();
  DlssReadiness r;
  r.bridge = !bridge_source(runtime).empty();
  r.dlss_model = file(settings.nvidia_dir / "nvngx_dlss.dll") || file(beside / "nvngx_dlss.dll");
  r.cheeky_package = file(settings.cheeky_dir / "CheekyVulkanLayer.dll") &&
                     file(settings.cheeky_dir / "CheekyFoveatedDLSS" / "Vulkan" / "Cheeky.json");
  r.nr_model = file(settings.nvidia_dir / "nvngx_dlssnr.dll") ||
               file(beside / "CheekyFoveatedDLSS" / "nvngx_dlssnr.dll");
  return r;
}

std::vector<std::string> apply_dlss_optimization(const DlssOptimization& settings, std::string_view package,
                                                 const fs::path& runtime, Environment& environment) {
  std::vector<std::string> problems;
  const auto p = settings.profile(package);
  if (!p.enabled) return problems;
  const auto ready = dlss_readiness(settings, runtime);
  if (!ready.bridge) problems.push_back("venture_ngx.dll is missing beside the runtime: DLSS stays off.");
  if (!ready.dlss_model) problems.push_back("nvngx_dlss.dll was not found: set the NVIDIA DLLs folder.");
  if (const auto bridge = bridge_source(runtime); !bridge.empty() && bridge.parent_path() != runtime.parent_path()) {
    std::error_code ec;
    fs::copy_file(bridge, runtime.parent_path() / "venture_ngx.dll", fs::copy_options::overwrite_existing, ec);
    if (ec) problems.push_back(std::format("venture_ngx.dll could not be copied beside the runtime: {}", ec.message()));
  }
  environment.push_back({"VENTURE_EXPERIMENTAL_RENDER_SCALE", std::format("{:.2f}", p.source_percent / 100.0)});
  environment.push_back({"VENTURE_EXPERIMENTAL_DLSS", "1"});
  if (file(settings.nvidia_dir / "nvngx_dlss.dll"))
    environment.push_back({"VENTURE_DLSS_PATH", paths::utf8(settings.nvidia_dir)});
  if (!p.cheeky) return problems;
  if (!ready.cheeky_package) {
    problems.push_back("The Cheeky package folder is not set or incomplete: Cheeky stays off.");
    return problems;
  }
#ifdef _WIN32
  // Cheeky's layer activates only from the folder of the executable it runs in.
  const auto beside = runtime.parent_path();
  const auto target = beside / "CheekyFoveatedDLSS";
  try {
    const auto options = fs::copy_options::overwrite_existing;
    fs::copy_file(settings.cheeky_dir / "CheekyVulkanLayer.dll", beside / "CheekyVulkanLayer.dll", options);
    fs::create_directories(target / "Vulkan");
    for (const char* name : {"CheekyFoveatedDLSSHost.dll", "CheekyFoveatedDLSSRuntime.dll"})
      fs::copy_file(settings.cheeky_dir / "CheekyFoveatedDLSS" / name, target / name, options);
    fs::copy_file(settings.cheeky_dir / "CheekyFoveatedDLSS" / "Vulkan" / "Cheeky.json", target / "Vulkan" / "Cheeky.json",
                  options);
    if (p.neural_rendering && file(settings.nvidia_dir / "nvngx_dlssnr.dll") &&
        !file(target / "nvngx_dlssnr.dll"))
      fs::copy_file(settings.nvidia_dir / "nvngx_dlssnr.dll", target / "nvngx_dlssnr.dll");
    if (p.neural_rendering && !file(target / "nvngx_dlssnr.dll"))
      problems.push_back("nvngx_dlssnr.dll was not found: Neural Rendering stays off.");
    merge_ini(target / "CheekyFoveatedDLSS.ini", cheeky_values(p, false));
    if (p.nr_thumbstick) environment.push_back({"VENTURE_NR_THUMBSTICK", "1"});
    // Toggle NR starts from the launch state (xr/cheeky_live.h).
    if (p.neural_rendering) environment.push_back({"VENTURE_CHEEKY_NR", "1"});
    environment.push_back({"VK_ADD_IMPLICIT_LAYER_PATH", paths::utf8(target / "Vulkan")});
  } catch (const std::exception& e) {
    log::warn("dlss", "Cheeky could not be deployed beside the runtime: {}", e.what());
    problems.push_back(std::format("Cheeky could not be deployed beside the runtime: {}", e.what()));
  }
#else
  problems.push_back("Cheeky's Vulkan layer is Windows only.");
#endif
  return problems;
}
std::string apply_in_game(const DlssOptimization& settings, std::string_view package, const fs::path& runtime,
                          std::string& error) {
  const auto target = runtime.parent_path() / "CheekyFoveatedDLSS";
  if (!file(target / "CheekyFoveatedDLSSHost.dll")) {
    error = "Cheeky is not deployed beside the runtime: launch the game with Cheeky first.";
    return {};
  }
  const auto p = settings.profile(package);
  try {
    // Kept for the next launch as well.
    merge_ini(target / "CheekyFoveatedDLSS.ini", cheeky_values(p, false));
    const auto id = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count());
    std::string command = "1\n" + id + "\nset\n";
    for (const auto& [key, value] : cheeky_values(p, true)) command += key + "=" + value + "\n";
    paths::atomic_write(target / "venture-live-command.txt", command);
    return id;
  } catch (const std::exception& e) {
    error = e.what();
    return {};
  }
}

std::optional<bool> in_game_status(const fs::path& runtime, std::string_view request) {
  std::ifstream in(runtime.parent_path() / "CheekyFoveatedDLSS" / "venture-live-status.txt");
  std::string id;
  int applied = -1;
  if (!(in >> id >> applied) || id != request) return std::nullopt;
  return applied == 1;
}
}  // namespace venture::launcher
