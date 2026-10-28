#include "common/paths.h"
#include "common/platform/win32/native_path.h"
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <shlobj.h>
#include <vector>

namespace venture::paths {
namespace {
struct File {
  HANDLE handle;
  ~File() {
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
  }
};
} // namespace
fs::path io_path(const fs::path &file) { return platform::native_path(file); }
std::string read(const fs::path &file, size_t limit) {
  // CreateFileW FILE_SHARE_DELETE lets a rename publish a new manifest
  // while this reader retains its handle to the complete old file.
  File input{CreateFileW(platform::native_path(file).c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
  if (input.handle == INVALID_HANDLE_VALUE)
    throw std::runtime_error("Could not read local file.");
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(input.handle, &size) || size.QuadPart < 0 ||
      std::uint64_t(size.QuadPart) > limit)
    throw std::runtime_error("Local file is too large.");
  std::string text(static_cast<size_t>(size.QuadPart), '\0');
  for (size_t offset = 0; offset < text.size();) {
    DWORD count = 0;
    const auto chunk = static_cast<DWORD>(
        std::min<size_t>(text.size() - offset, MAXDWORD));
    if (!ReadFile(input.handle, text.data() + offset, chunk, &count, nullptr) ||
        !count)
      throw std::runtime_error("Could not read local file.");
    offset += count;
  }
  return text;
}
fs::path data_dir() {
  // VENTURE_DATA_DIR relocates all state (tests, portable installs).
  if (const wchar_t* custom = _wgetenv(L"VENTURE_DATA_DIR"); custom && *custom)
    return fs::path(custom);
  constexpr auto legacy_env = L"QUESTPORT_DATA_DIR"; // rebrand: legacy override
  if (const wchar_t* custom = _wgetenv(legacy_env); custom && *custom)
    return fs::path(custom);
  static const fs::path directory = [] {
    PWSTR folder = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder)))
      throw std::runtime_error("Windows could not locate Local AppData.");
    fs::path base(folder);
    CoTaskMemFree(folder);
    constexpr auto legacy_name = L"QuestPort"; // rebrand: legacy default directory
    return migrate_data_dir(base / L"Venture", base / legacy_name);
  }();
  return directory;
}
fs::path executable() {
  std::wstring path(32768, L'\0');
  auto n = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
  if (!n || n == path.size())
    throw std::runtime_error("Could not locate executable.");
  path.resize(n);
  return fs::path(path);
}
void atomic_write(const fs::path &file, std::string_view text) {
  fs::create_directories(file.parent_path());
  auto tmp = file;
  tmp += L".tmp";
  {
    std::ofstream out(platform::native_path(tmp).c_str(), std::ios::binary | std::ios::trunc);
    out.write(text.data(), text.size());
    out.flush();
    if (!out)
      throw std::runtime_error("Could not save local file. Check free space "
                               "and folder permissions.");
  }
  replace_file(tmp, file);
}
void replace_file(const fs::path &source, const fs::path &destination) {
  File input{CreateFileW(platform::native_path(source).c_str(), DELETE | SYNCHRONIZE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
  const auto target = platform::native_path(destination);
  if (input.handle == INVALID_HANDLE_VALUE || target.size() > 32767)
    throw std::runtime_error(
        "Could not replace local file. Check folder permissions.");
  const auto bytes = target.size() * sizeof(wchar_t);
  std::vector<std::byte> buffer(sizeof(FILE_RENAME_INFO) + bytes);
  auto* rename = std::construct_at(
      reinterpret_cast<FILE_RENAME_INFO*>(buffer.data()));
  // Windows 10 RS1+ POSIX rename keeps existing readers on the old file and
  // subsequent opens on the new file, with no missing-name interval.
  // https://learn.microsoft.com/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_rename_information
  rename->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
  rename->FileNameLength = static_cast<DWORD>(bytes);
  std::memcpy(rename->FileName, target.data(), bytes);
  if (SetFileInformationByHandle(input.handle, FileRenameInfoEx, rename,
                                 static_cast<DWORD>(buffer.size())))
    return;
  const auto error = GetLastError();
  // Older Windows/filesystems can still replace files without open readers.
  if (error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_SUPPORTED ||
      error == ERROR_CALL_NOT_IMPLEMENTED) {
    rename->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS;
    if (SetFileInformationByHandle(input.handle, FileRenameInfo, rename,
                                   static_cast<DWORD>(buffer.size())))
      return;
  }
  throw std::runtime_error(
      "Could not replace local file. Check folder permissions.");
}
} // namespace venture::paths
