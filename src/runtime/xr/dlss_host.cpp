// The parts of DLSS (xr/dlss.h) that Vulkan instance and device creation need
// before any XR session: the opt-in, the render scale and NGX's extensions.
// In venture_host_xr so that venture_vulkan can use them.
#include "xr/dlss.h"
#include "xr/dlss_bridge.h"
#include "common/environment.h"
#include "common/log.h"
#include <cmath>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#include <array>
#endif

namespace venture::xr::dlss {
double render_scale() {
  static const double scale = [] {
    const char* text = venture::environment::getenv("VENTURE_EXPERIMENTAL_RENDER_SCALE");
    if (!text || !*text) return 1.0;
    char* end = nullptr;
    const double value = std::strtod(text, &end);
    if (end == text || !std::isfinite(value) || value < 0.25 || value >= 1.0) {
      log::warn("xr", "VENTURE_EXPERIMENTAL_RENDER_SCALE={} ignored; expected a value in [0.25, 1)", text);
      return 1.0;
    }
    log::info("xr", "experimental render scale {:.3f}: projection chains are scaled up at presentation", value);
    return value;
  }();
  return scale;
}

#ifndef _WIN32
bool requested() { return false; }
std::vector<const char*> required_extensions(bool) { return {}; }
#else
std::wstring runtime_directory() {
  std::array<wchar_t, 4096> path{};
  const auto length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
  std::wstring text(path.data(), length < path.size() ? length : 0);
  const auto slash = text.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring(L".") : text.substr(0, slash);
}

const Bridge* bridge() {
  static const Bridge* loaded = []() -> const Bridge* {
    const auto path = runtime_directory() + L"\\venture_ngx.dll";
    const auto module = LoadLibraryW(path.c_str());
    if (!module) {
      log::warn("dlss", "venture_ngx.dll did not load from the runtime directory (error {}); DLSS is off",
                GetLastError());
      return nullptr;
    }
    static Bridge b;
    const auto proc = [&](auto& out, const char* name) {
      out = reinterpret_cast<std::remove_reference_t<decltype(out)>>(
          reinterpret_cast<void*>(GetProcAddress(module, name)));
      return out != nullptr;
    };
    if (!proc(b.required_extensions, "venture_ngx_required_extensions") || !proc(b.init, "venture_ngx_init") ||
        !proc(b.available, "venture_ngx_dlss_available") || !proc(b.create, "venture_ngx_dlss_create") ||
        !proc(b.evaluate, "venture_ngx_dlss_evaluate") || !proc(b.release, "venture_ngx_release") ||
        !proc(b.shutdown, "venture_ngx_shutdown")) {
      log::warn("dlss", "venture_ngx.dll lacks an export; DLSS is off");
      return nullptr;
    }
    log::info("dlss", "venture_ngx.dll loaded");
    return &b;
  }();
  return loaded;
}

bool requested() {
  static const bool value = [] {
    const char* text = venture::environment::getenv("VENTURE_EXPERIMENTAL_DLSS");
    return text && *text && *text != '0' && render_scale() < 1.0;
  }();
  return value;
}

std::vector<const char*> required_extensions(bool device) {
  if (!requested()) return {};
  auto* b = bridge();
  if (!b) return {};
  std::uint32_t instance_count{}, device_count{};
  const char** instance_names{};
  const char** device_names{};
  if (!succeeded(b->required_extensions(&instance_count, &instance_names, &device_count, &device_names))) {
    log::warn("dlss", "NGX extension query failed");
    return {};
  }
  return device ? std::vector<const char*>(device_names, device_names + device_count)
                : std::vector<const char*>(instance_names, instance_names + instance_count);
}
#endif
}  // namespace venture::xr::dlss
