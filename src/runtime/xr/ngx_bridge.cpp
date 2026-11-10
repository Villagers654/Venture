#include "xr/ngx_bridge.h"
#include "common/environment.h"
#include "common/log.h"
#include <array>
#include <type_traits>
#ifdef _WIN32
#include <windows.h>
#endif

namespace venture::xr::ngx_bridge {
bool requested() {
  static const bool value = [] {
    const char* text = venture::environment::getenv("VENTURE_NR");
    return text && *text == '1';
  }();
  return value;
}
#ifndef _WIN32
std::vector<const char*> required_extensions(bool) { return {}; }
#else
std::wstring runtime_directory() {
  std::array<wchar_t, 4096> path{};
  const auto length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
  std::wstring text(path.data(), length < path.size() ? length : 0);
  const auto slash = text.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring(L".") : text.substr(0, slash);
}
const Bridge* get() {
  static const Bridge* loaded = []() -> const Bridge* {
    const auto path = runtime_directory() + L"\\venture_ngx.dll";
    const auto module = LoadLibraryW(path.c_str());
    if (!module) {
      log::warn("venture-nr", "venture_ngx.dll did not load from the runtime directory (error {}); Venture NR is off",
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
        !proc(b.release, "venture_ngx_release") || !proc(b.shutdown, "venture_ngx_shutdown") ||
        !proc(b.nr_create, "venture_ngx_nr_create") || !proc(b.nr_evaluate, "venture_ngx_nr_evaluate")) {
      log::warn("venture-nr", "venture_ngx.dll lacks an export; Venture NR is off");
      return nullptr;
    }
    log::info("venture-nr", "venture_ngx.dll loaded");
    return &b;
  }();
  return loaded;
}
std::vector<const char*> required_extensions(bool device) {
  if (!requested()) return {};
  auto* b = get();
  if (!b) return {};
  std::uint32_t instance_count{}, device_count{};
  const char** instance_names{};
  const char** device_names{};
  if (!succeeded(b->required_extensions(&instance_count, &instance_names, &device_count, &device_names))) {
    log::warn("venture-nr", "NGX extension query failed");
    return {};
  }
  return device ? std::vector<const char*>(device_names, device_names + device_count)
                : std::vector<const char*>(instance_names, instance_names + instance_count);
}
#endif
}  // namespace venture::xr::ngx_bridge
