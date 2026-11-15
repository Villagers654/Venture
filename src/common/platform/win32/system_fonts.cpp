#include "common/system_fonts.h"
#include <windows.h>

#include <shlobj.h>
#include <vector>

namespace venture::system_fonts {
namespace {
std::filesystem::path known_folder(const KNOWNFOLDERID& id) {
  PWSTR folder = nullptr;
  std::filesystem::path out;
  if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &folder))) out = folder;
  CoTaskMemFree(folder);
  return out;
}
// The machine's Fonts folder, then the per-user one that Settings installs to.
const std::vector<std::filesystem::path>& font_folders() {
  static const std::vector<std::filesystem::path> folders = [] {
    std::vector<std::filesystem::path> out;
    if (auto fonts = known_folder(FOLDERID_Fonts); !fonts.empty()) out.push_back(fonts);
    if (auto local = known_folder(FOLDERID_LocalAppData); !local.empty())
      out.push_back(local / L"Microsoft" / L"Windows" / L"Fonts");
    return out;
  }();
  return folders;
}
}  // namespace

std::optional<Face> find(const Query& query) {
  for (auto name : query.windows_files)
    for (const auto& folder : font_folders()) {
      Face face{folder / std::filesystem::path(std::u8string(name.begin(), name.end())), 0};
      std::error_code ec;
      if (!std::filesystem::is_regular_file(face.file, ec)) continue;
      if (!query.sample || covers(face, query.sample)) return face;
    }
  return std::nullopt;
}
}  // namespace venture::system_fonts
