#include "launcher/obs_mirror.h"
#include "common/json.h"
#include <algorithm>
#include <cctype>

#ifdef _WIN32
#include <windows.h>
#endif

namespace venture::launcher {
namespace {
constexpr std::string_view kLayerName = "XR_APILAYER_NOVENDOR_OBSMirror";

bool contains_obs_mirror(std::string text) {
  std::ranges::transform(text, text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return text.find("obsmirror") != std::string::npos;
}
}  // namespace

ObsMirror find_obs_mirror(const std::vector<ImplicitLayer>& layers) {
  ObsMirror found;
  for (const auto& layer : layers) {
    std::error_code ec;
    Json manifest;
    try {
      // Manifests written by PowerShell installers can start with a UTF-8 BOM.
      const auto bytes = paths::read(layer.manifest, 1024 * 1024);
      std::string_view text = bytes;
      if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
      manifest = parse_json(text);
    } catch (const std::exception&) {
      continue;
    }
    const auto api = manifest.find("api_layer");
    if (api == manifest.end() || !api->is_object()) continue;
    const auto name = str(*api, "name");
    if (name != kLayerName && !contains_obs_mirror(paths::utf8(layer.manifest.filename()))) continue;
    // A relative library path is relative to the manifest (OpenXR loader).
    auto library = paths::from_utf8(str(*api, "library_path"));
    if (library.is_relative()) library = layer.manifest.parent_path() / library;
    const auto state = layer.disabled                   ? ObsMirror::State::disabled
                       : fs::is_regular_file(library, ec) ? ObsMirror::State::ready
                                                          : ObsMirror::State::missing;
    if (found.manifest.empty() || state > found.state)
      found = {state, layer.manifest, str(*api, "implementation_version")};
  }
  return found;
}

std::vector<ImplicitLayer> implicit_openxr_layers() {
  std::vector<ImplicitLayer> out;
#ifdef _WIN32
  for (const auto root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
    HKEY key;
    if (RegOpenKeyExW(root, L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit", 0, KEY_READ, &key) != ERROR_SUCCESS)
      continue;
    for (DWORD i = 0;; ++i) {
      wchar_t name[2048];
      DWORD name_size = DWORD(std::size(name)), type = 0, data = 0, data_size = sizeof data;
      const LONG r = RegEnumValueW(key, i, name, &name_size, nullptr, &type, reinterpret_cast<BYTE*>(&data),
                                   &data_size);
      if (r == ERROR_NO_MORE_ITEMS) break;
      if (r == ERROR_SUCCESS) out.push_back({fs::path(name), type == REG_DWORD && data != 0});
    }
    RegCloseKey(key);
  }
#endif
  return out;
}
}  // namespace venture::launcher
