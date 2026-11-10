#include "launcher/vr_optimization.h"
#include "common/json.h"
#include "common/log.h"
#include <algorithm>
#include <format>
#include <fstream>
#ifdef _WIN32
#include <windows.h>
#include <dxgi.h>
#include <shellapi.h>
#endif

namespace venture::launcher {
namespace {
constexpr const char* kFile = "vr-optimization.json";

Json to_json(const VrProfile& p) {
  return Json{{"mode", p.mode},
              {"preset", p.preset},
              {"style", p.style},
              {"intensity", p.intensity},
              {"localTone", p.local_tone},
              {"localStructure", p.local_structure},
              {"skinStructure", p.skin_structure},
              {"nrWidth", p.nr_width},
              {"nrHeight", p.nr_height},
              {"eyeTracking", p.eye_tracking},
              {"fixedDistance", p.fixed_distance},
              {"debug", p.debug},
              {"workingScale", p.working_scale},
              {"cache", p.cache},
              {"interval", p.interval},
              {"transition", p.transition},
              {"roundness", p.roundness},
              {"gpuBudgetTenths", p.gpu_budget_tenths},
              {"cacheMaxRotation", p.cache_max_rotation},
              {"split", p.split},
              {"motion", p.motion},
              {"useDepth", p.use_depth},
              {"mirror", p.mirror},
              {"gazePerEye", p.gaze_per_eye},
              {"gazeSmoothing", p.gaze_smoothing},
              {"gazeQuantization", p.gaze_quantization},
              {"gazeJumpReset", p.gaze_jump_reset},
              {"gazeHoldMs", p.gaze_hold_ms},
              {"motionScaleX", p.motion_scale_x},
              {"motionScaleY", p.motion_scale_y},
              {"flowRadius", p.flow_radius},
              {"flowPenalty", p.flow_penalty},
              {"vrs", p.vrs},
              {"vrsDebug", p.vrs_debug},
              {"vrsWidth", p.vrs_width},
              {"vrsHeight", p.vrs_height},
              {"vrsRate", p.vrs_rate},
              {"vrsMargin", p.vrs_margin},
              {"vrsGazeSmoothing", p.vrs_gaze_smoothing},
              {"vrsGazeHoldMs", p.vrs_gaze_hold_ms},
              {"supersample", p.supersample},
              {"overlay", p.overlay},
              {"nrToggle", p.nr_toggle},
              {"nrShortcut", p.nr_shortcut},
              {"nrShortcutMs", p.nr_shortcut_ms}};
}

VrProfile from_json(const Json& j) {
  VrProfile p;
  if (!j.is_object()) return p;
  const auto num = [&](const char* key, int& out, int lo, int hi) { out = std::clamp(j.value(key, out), lo, hi); };
  const auto flag = [&](const char* key, bool& out) { out = j.value(key, out); };
  num("mode", p.mode, 0, 2);
  num("preset", p.preset, 0, 2);
  num("style", p.style, 0, 2);
  num("intensity", p.intensity, 0, 100);
  num("localTone", p.local_tone, 0, 200);
  num("localStructure", p.local_structure, 0, 200);
  num("skinStructure", p.skin_structure, 0, 200);
  num("nrWidth", p.nr_width, 10, 100);
  num("nrHeight", p.nr_height, 10, 100);
  flag("eyeTracking", p.eye_tracking);
  num("fixedDistance", p.fixed_distance, 0, 1000);
  flag("debug", p.debug);
  num("workingScale", p.working_scale, 25, 100);
  flag("cache", p.cache);
  num("interval", p.interval, 2, 8);
  num("transition", p.transition, 0, 50);
  num("roundness", p.roundness, 0, 100);
  num("gpuBudgetTenths", p.gpu_budget_tenths, 0, 500);
  num("cacheMaxRotation", p.cache_max_rotation, 0, 300);
  flag("split", p.split);
  flag("motion", p.motion);
  flag("useDepth", p.use_depth);
  flag("mirror", p.mirror);
  flag("gazePerEye", p.gaze_per_eye);
  num("gazeSmoothing", p.gaze_smoothing, 0, 95);
  num("gazeQuantization", p.gaze_quantization, 1, 64);
  num("gazeJumpReset", p.gaze_jump_reset, 0, 100);
  num("gazeHoldMs", p.gaze_hold_ms, 0, 2000);
  num("motionScaleX", p.motion_scale_x, -200, 200);
  num("motionScaleY", p.motion_scale_y, -200, 200);
  num("flowRadius", p.flow_radius, 0, 8);
  num("flowPenalty", p.flow_penalty, 0, 200);
  flag("vrs", p.vrs);
  flag("vrsDebug", p.vrs_debug);
  num("vrsWidth", p.vrs_width, 5, 100);
  num("vrsHeight", p.vrs_height, 5, 100);
  p.vrs_rate = j.value("vrsRate", p.vrs_rate) >= 4 ? 4 : 2;
  num("vrsMargin", p.vrs_margin, 0, 40);
  num("vrsGazeSmoothing", p.vrs_gaze_smoothing, 0, 95);
  num("vrsGazeHoldMs", p.vrs_gaze_hold_ms, 0, 2000);
  num("supersample", p.supersample, 100, 150);
  flag("overlay", p.overlay);
  flag("nrToggle", p.nr_toggle);
  num("nrShortcut", p.nr_shortcut, 0, 2);
  num("nrShortcutMs", p.nr_shortcut_ms, 500, 5000);
  return p;
}

bool file(const fs::path& path) {
  std::error_code ec;
  return !path.empty() && fs::is_regular_file(path, ec);
}

#ifdef _WIN32
// Writes DWORD values under HKCU\<subkey>.
bool write_dwords(const wchar_t* subkey, std::span<const std::pair<const wchar_t*, int>> values, std::string_view what,
                  std::string& error) {
  HKEY key{};
  const LSTATUS opened =
      RegCreateKeyExW(HKEY_CURRENT_USER, subkey, 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
  if (opened != ERROR_SUCCESS) {
    error = std::format("{} settings could not be written (registry error {})", what, opened);
    return false;
  }
  bool ok = true;
  for (const auto& [name, value] : values) {
    const DWORD data = static_cast<DWORD>(value);
    ok &= RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&data), sizeof(data)) == ERROR_SUCCESS;
  }
  RegCloseKey(key);
  if (!ok) error = std::format("Some {} settings could not be written", what);
  return ok;
}
#endif

void set(Environment& environment, std::string name, std::string value) {
  const auto it = std::ranges::find_if(environment, [&](const auto& e) { return e.first == name; });
  if (it != environment.end()) it->second = std::move(value);
  else environment.push_back({std::move(name), std::move(value)});
}
}  // namespace

VrOptimization VrOptimization::load(const fs::path& data) {
  VrOptimization out;
  const auto path = data / kFile;
  if (!file(path)) return out;
  try {
    const auto j = paths::read_json(path);
    if (const auto it = j.find("runtimeJson"); it != j.end() && it->is_string())
      out.runtime_json = paths::from_utf8(it->get<std::string>());
    if (const auto it = j.find("nvidiaDir"); it != j.end() && it->is_string())
      out.nvidia_dir = paths::from_utf8(it->get<std::string>());
    if (const auto it = j.find("games"); it != j.end() && it->is_object())
      for (const auto& [package, value] : it->items()) out.games[package] = from_json(value);
  } catch (const std::exception& e) {
    log::warn("vr-optimization", "{} could not be read: {}", kFile, e.what());
  }
  return out;
}

void VrOptimization::save(const fs::path& data) const {
  Json games = Json::object();
  for (const auto& [package, profile] : this->games) games[package] = to_json(profile);
  const Json j{{"runtimeJson", paths::utf8(runtime_json)}, {"nvidiaDir", paths::utf8(nvidia_dir)}, {"games", games}};
  paths::atomic_write(data / kFile, j.dump(2));
}

VrProfile VrOptimization::profile(std::string_view package) const {
  const auto it = games.find(package);
  return it == games.end() ? VrProfile{} : it->second;
}

VrReadiness vr_readiness(const VrOptimization& settings, const fs::path& runtime) {
  const auto beside = runtime.parent_path();
  return {.bridge = file(beside / "venture_ngx.dll"),
          .nr_model = file(settings.nvidia_dir / "nvngx_dlssnr.dll") || file(beside / "nvngx_dlssnr.dll"),
          .vdxr = file(settings.runtime_json)};
}

bool write_vdxr_settings(const VrProfile& p, std::string& error) {
#ifdef _WIN32
  // VDXR's DWORD settings (the names its getSetting() reads).
  const std::pair<const wchar_t*, int> values[] = {
      {L"DLSSNR_Enabled", p.vdxr_nr() ? 1 : 0},
      {L"DLSSNR_Bypass", 0},  // NR on at launch and after Apply in game; Toggle NR bypasses it in game
      {L"DLSSNR_Transition", p.transition},
      {L"DLSSNR_Preset", p.preset},
      {L"DLSSNR_Style", p.style},
      {L"DLSSNR_Intensity", p.intensity},
      {L"DLSSNR_LocalToneStrength", p.local_tone},
      {L"DLSSNR_LocalStructureStrength", p.local_structure},
      {L"DLSSNR_SkinStructureStrength", p.skin_structure},
      {L"FoveationSize", std::max(p.nr_width, p.nr_height)},
      {L"DLSSNR_FoveationWidth", p.nr_width},
      {L"DLSSNR_FoveationHeight", p.nr_height},
      {L"DLSSNR_EyeTracking", p.eye_tracking ? 1 : 0},
      {L"DLSSNR_FixedDistance", p.fixed_distance},
      {L"DLSSNR_DebugFovea", p.debug ? 1 : 0},
      {L"DLSSNR_DebugSplit", p.split ? 1 : 0},
      {L"DLSSNR_DebugVrs", p.vrs && p.vrs_debug ? 1 : 0},
      {L"mirror_window", p.mirror ? 1 : 0},
      {L"DLSSNR_VrsWidth", p.vrs_width},
      {L"DLSSNR_VrsHeight", p.vrs_height},
      {L"DLSSNR_VrsMargin", p.vrs_margin},
      {L"DLSSNR_Motion", p.motion ? 1 : 0},
      {L"DLSSNR_UseDepth", p.use_depth ? 1 : 0},
      {L"DLSSNR_WorkingScale", p.working_scale},
      {L"DLSSNR_Cache", !p.cache ? 0 : p.gpu_budget_tenths > 0 ? 2 : 1},
      {L"DLSSNR_CacheMaxInterval", p.interval},
      {L"DLSSNR_CacheGpuBudget", p.gpu_budget_tenths},
      {L"DLSSNR_CacheMaxRotation", p.cache_max_rotation},
      {L"DLSSNR_GazePerEye", p.gaze_per_eye ? 1 : 0},
      {L"DLSSNR_GazeSmoothing", p.gaze_smoothing},
      {L"DLSSNR_GazeQuantization", p.gaze_quantization},
      {L"DLSSNR_GazeJumpReset", p.gaze_jump_reset},
      {L"DLSSNR_GazeHoldMs", p.gaze_hold_ms},
      {L"DLSSNR_MotionScaleX", p.motion_scale_x},
      {L"DLSSNR_MotionScaleY", p.motion_scale_y},
      {L"DLSSNR_FlowRadius", p.flow_radius},
      {L"DLSSNR_FlowPenalty", p.flow_penalty}};
  return write_dwords(L"SOFTWARE\\VirtualDesktop-OpenXR\\DLSSNR", values, "VDXR", error);
#else
  (void)p;
  error = "VDXR is Windows only";
  return false;
#endif
}

bool write_venture_vrs(const VrProfile& p, std::string& error) {
#ifdef _WIN32
  // Read live by the runtime (hle/vulkan/render_area.cpp).
  const std::pair<const wchar_t*, int> values[] = {{L"On", p.vrs ? 1 : 0},
                                                   {L"Width", p.vrs_width},
                                                   {L"Height", p.vrs_height},
                                                   {L"Rate", p.vrs_rate},
                                                   {L"Distance", p.fixed_distance},
                                                   {L"Margin", p.vrs_margin},
                                                   {L"GazeSmoothing", p.vrs_gaze_smoothing},
                                                   {L"GazeHoldMs", p.vrs_gaze_hold_ms},
                                                   {L"Debug", p.vrs && p.vrs_debug ? 1 : 0}};
  return write_dwords(L"SOFTWARE\\Venture\\VRS", values, "VRS", error);
#else
  (void)p;
  error = "VRS live settings are Windows only";
  return false;
#endif
}

bool write_venture_nr(const VrProfile& p, std::string& error) {
#ifdef _WIN32
  // Read live by the runtime (xr/venture_nr.cpp).
  const std::pair<const wchar_t*, int> values[] = {{L"On", 1},
                                                   {L"Width", p.nr_width},
                                                   {L"Height", p.nr_height},
                                                   {L"Preset", p.preset},
                                                   {L"Style", p.style},
                                                   {L"Intensity", p.intensity},
                                                   {L"Tone", p.local_tone},
                                                   {L"Structure", p.local_structure},
                                                   {L"Skin", p.skin_structure},
                                                   {L"Debug", p.debug ? 1 : 0},
                                                   {L"WorkingScale", p.working_scale},
                                                   {L"Cache", p.cache ? 1 : 0},
                                                   {L"Interval", p.interval},
                                                   {L"Transition", p.transition},
                                                   {L"Roundness", p.roundness},
                                                   {L"Split", p.split ? 1 : 0}};
  return write_dwords(L"SOFTWARE\\Venture\\NR", values, "Venture NR", error);
#else
  (void)p;
  error = "Venture NR is Windows only";
  return false;
#endif
}

bool enable_steamvr_runtime(std::string& error) {
#ifdef _WIN32
  // SteamVR's OpenXR manifest, in one of the Steam libraries.
  std::vector<fs::path> libraries;
  wchar_t steam[MAX_PATH]{};
  DWORD size = sizeof(steam);
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, steam, &size) ==
      ERROR_SUCCESS) {
    const fs::path root(steam);
    libraries.push_back(root);
    std::ifstream in(root / "steamapps" / "libraryfolders.vdf");
    for (std::string line; std::getline(in, line);) {
      const auto key = line.find("\"path\"");
      if (key == std::string::npos) continue;
      const auto open = line.find('"', key + 6), close = open == std::string::npos ? open : line.rfind('"');
      if (open == std::string::npos || close <= open) continue;
      std::string value = line.substr(open + 1, close - open - 1);
      for (std::size_t at = 0; (at = value.find("\\\\", at)) != std::string::npos; ++at) value.erase(at, 1);
      libraries.push_back(paths::from_utf8(value));
    }
  }
  fs::path manifest;
  for (const auto& library : libraries)
    if (const auto candidate = library / "steamapps" / "common" / "SteamVR" / "steamxr_win64.json"; file(candidate)) {
      manifest = candidate.lexically_normal();
      break;
    }
  if (manifest.empty()) {
    error = "SteamVR was not found in the Steam libraries";
    return false;
  }
  // HKLM needs administrator rights: reg.exe, elevated (Windows asks).
  const std::wstring arguments = L"add \"HKLM\\SOFTWARE\\Khronos\\OpenXR\\1\" /v ActiveRuntime /t REG_SZ /d \"" +
                                 manifest.wstring() + L"\" /f /reg:64";
  SHELLEXECUTEINFOW info{sizeof(info)};
  info.fMask = SEE_MASK_NOCLOSEPROCESS;
  info.lpVerb = L"runas";
  info.lpFile = L"reg.exe";
  info.lpParameters = arguments.c_str();
  info.nShow = SW_HIDE;
  if (!ShellExecuteExW(&info) || !info.hProcess) {
    error = "The change was not allowed";
    return false;
  }
  WaitForSingleObject(info.hProcess, 60000);
  DWORD code = 1;
  GetExitCodeProcess(info.hProcess, &code);
  CloseHandle(info.hProcess);
  if (code != 0) {
    error = std::format("SteamVR could not be made the active runtime (reg.exe exit {})", code);
    return false;
  }
  log::info("vr-optimization", "SteamVR is now the active OpenXR runtime ({})", paths::utf8(manifest));
  return true;
#else
  error = "Windows only";
  return false;
#endif
}

VrSystemInfo detect_vr_system() {
  VrSystemInfo out;
#ifdef _WIN32
  // The GPU, through DXGI loaded at run time (the launcher does not link it).
  if (HMODULE dxgi = LoadLibraryW(L"dxgi.dll")) {
    using Create = HRESULT(WINAPI*)(REFIID, void**);
    if (auto create = reinterpret_cast<Create>(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")))) {
      IDXGIFactory1* factory = nullptr;
      if (SUCCEEDED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) && factory) {
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
          DXGI_ADAPTER_DESC1 desc{};
          const bool hardware = SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE);
          adapter->Release();
          if (!hardware) continue;
          out.nvidia |= desc.VendorId == 0x10DE;
          out.amd |= desc.VendorId == 0x1002;
          if (out.gpu.empty() || desc.VendorId == 0x10DE) {
            out.gpu = paths::utf8(fs::path(desc.Description));
            out.rtx |= desc.VendorId == 0x10DE && out.gpu.find("RTX") != std::string::npos;
          }
        }
        factory->Release();
      }
    }
    FreeLibrary(dxgi);
  }
  // Virtual Desktop's streamer.
  wchar_t programs[MAX_PATH]{};
  if (GetEnvironmentVariableW(L"ProgramFiles", programs, MAX_PATH)) {
    std::error_code ec;
    out.virtual_desktop = fs::exists(fs::path(programs) / L"Virtual Desktop Streamer", ec);
  }
  // The active OpenXR runtime.
  wchar_t runtime[1024]{};
  DWORD size = sizeof(runtime);
  if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime", RRF_RT_REG_SZ, nullptr,
                   runtime, &size) == ERROR_SUCCESS) {
    std::wstring name(runtime);
    for (auto& c : name) c = wchar_t(towlower(c));
    out.steamvr = name.find(L"steamxr") != std::wstring::npos || name.find(L"steamvr") != std::wstring::npos;
  }
#endif
  return out;
}

std::vector<std::string> apply_vr_optimization(const VrOptimization& settings, std::string_view package,
                                               const fs::path& runtime, Environment& environment) {
  std::vector<std::string> problems;
  if (!settings.games.contains(package)) return problems;
  const auto p = settings.profile(package);
  std::string error;
  if (p.overlay) environment.push_back({"VENTURE_STATS_OVERLAY", "1"});
  // Foveated VRS is Venture's: it applies with any runtime.
  if (p.vrs) {
    environment.push_back({"VENTURE_VRS", "1"});
    environment.push_back({"VENTURE_VRS_WIDTH", std::format("{:.2f}", p.vrs_width / 100.0)});
    environment.push_back({"VENTURE_VRS_HEIGHT", std::format("{:.2f}", p.vrs_height / 100.0)});
    environment.push_back({"VENTURE_VRS_RATE", std::to_string(p.vrs_rate)});
    environment.push_back({"VENTURE_VRS_DISTANCE", std::format("{:.2f}", p.fixed_distance / 100.0)});
  }
  // Eye tracked: VRS and Venture NR follow VDXR's NR region, or Venture's own gaze (OpenXR eye gaze, else
  // Virtual Desktop's shared eye tracking).
  if ((p.vrs || p.venture_nr()) && p.eye_tracking) environment.push_back({"VENTURE_VRS_GAZE", "1"});
  // The live values start as this game's (another game may have left others).
  if ((p.vrs || p.venture_nr()) && !write_venture_vrs(p, error)) problems.push_back(error);
  if (p.supersample > 100) set(environment, "VENTURE_RENDER_SCALE", std::format("{:.2f}", p.supersample / 100.0));
  // Toggle NR: the controller shortcut switches the selected NR (xr/nr_toggle.h).
  if (p.any_nr() && p.nr_toggle) {
    environment.push_back({"VENTURE_NR_THUMBSTICK", "1"});
    environment.push_back({"VENTURE_NR_SHORTCUT", p.nr_shortcut == 1 ? "left" : p.nr_shortcut == 2 ? "right" : "both"});
    environment.push_back({"VENTURE_NR_HOLD_MS", std::to_string(p.nr_shortcut_ms)});
  }
  if (p.any_nr()) environment.push_back({"VENTURE_OVERLAY_NR", "1"});
  if (p.venture_nr()) {
    const auto ready = vr_readiness(settings, runtime);
    if (!ready.bridge) problems.push_back("Venture NR: venture_ngx.dll is missing beside the runtime; NR stays off.");
    if (!ready.nr_model) problems.push_back("Venture NR: nvngx_dlssnr.dll was not found; set the NVIDIA DLLs folder.");
    environment.push_back({"VENTURE_NR", "1"});
    environment.push_back({"VENTURE_NR_TARGET", "venture"});
    if (file(settings.nvidia_dir / "nvngx_dlssnr.dll"))
      environment.push_back({"VENTURE_DLSS_PATH", paths::utf8(settings.nvidia_dir)});
    if (!write_venture_nr(p, error)) problems.push_back(error);
  }
  if (!p.vdxr_nr()) return problems;
  environment.push_back({"VENTURE_NR_TARGET", "vdxr"});
  if (!file(settings.runtime_json)) {
    problems.push_back(settings.runtime_json.empty()
                           ? std::string("VDXR DLSS NR: set the VDXR manifest; the system's OpenXR runtime is used.")
                           : std::format("VDXR DLSS NR: {} was not found; the system's OpenXR runtime is used.",
                                         paths::utf8(settings.runtime_json)));
    return problems;
  }
  // The loader reads it in the game's process only.
  set(environment, "XR_RUNTIME_JSON", paths::utf8(settings.runtime_json));
  if (!write_vdxr_settings(p, error)) problems.push_back(error);
  log::info("vr-optimization", "{} runs on {} (DLSS-NR)", package, paths::utf8(settings.runtime_json));
  return problems;
}

}  // namespace venture::launcher
