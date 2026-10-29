#include "common/file_ops.h"
#include "common/platform/win32/native_path.h"
#include <array>
#include <cstring>
#include <vector>
#include <windows.h>
#include <winioctl.h>

#include <bcrypt.h>

namespace venture {
struct FileLock::Impl {
  HANDLE handle = INVALID_HANDLE_VALUE;
  OVERLAPPED overlap{};
  ~Impl() {
    if (handle != INVALID_HANDLE_VALUE) {
      UnlockFileEx(handle, 0, 1, 0, &overlap);
      CloseHandle(handle);
    }
  }
};
FileLock::FileLock(const fs::path &file, bool wait) : impl_(std::make_unique<Impl>()) {
  if (is_link(file) || is_link(file.parent_path()))
    throw std::runtime_error("Cannot lock account data through a linked path.");
  fs::create_directories(file.parent_path());
  impl_->handle = CreateFileW(platform::native_path(file).c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (impl_->handle == INVALID_HANDLE_VALUE ||
      !LockFileEx(impl_->handle, LOCKFILE_EXCLUSIVE_LOCK | (wait ? 0 : LOCKFILE_FAIL_IMMEDIATELY), 0, 1, 0,
                  &impl_->overlap))
    throw std::runtime_error("Could not lock Venture account data.");
}
FileLock::~FileLock() = default;
std::string random_account_key() {
  std::array<unsigned char, 6> bytes{};
  if (BCryptGenRandom(nullptr, bytes.data(), bytes.size(),
                      BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
    throw std::runtime_error("Could not generate an account key.");
  std::string key;
  for (auto b : bytes) {
    key += "0123456789abcdef"[b >> 4];
    key += "0123456789abcdef"[b & 15];
  }
  return key;
}
bool is_link(const fs::path &path) {
  auto attributes = GetFileAttributesW(platform::native_path(path).c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    auto error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
      return false;
    throw std::runtime_error("Could not inspect local data directory.");
  }
  return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}
std::optional<FileIdentity> file_identity(const fs::path &path) {
  const auto handle = CreateFileW(platform::native_path(path).c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return std::nullopt;
  BY_HANDLE_FILE_INFORMATION info{};
  const auto ok = GetFileInformationByHandle(handle, &info);
  CloseHandle(handle);
  if (!ok || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
    return std::nullopt;
  const auto ticks = (std::uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) |
                     info.ftLastWriteTime.dwLowDateTime;
  return FileIdentity{info.dwVolumeSerialNumber,
      (std::uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow,
      (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow,
      static_cast<std::int64_t>(ticks / 10'000'000),
      static_cast<std::uint32_t>((ticks % 10'000'000) * 100)};
}
std::error_code move_directory(const fs::path &source, const fs::path &target) {
  if (MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH))
    return {};
  return {static_cast<int>(GetLastError()), std::system_category()};
}
namespace {
void erase(const fs::path &path) {
  const auto native = platform::native_path(path);
  auto attributes = GetFileAttributesW(native.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    auto error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
      return;
    throw std::runtime_error("Could not inspect local data for removal.");
  }
  if ((attributes & FILE_ATTRIBUTE_DIRECTORY) &&
      !(attributes & FILE_ATTRIBUTE_REPARSE_POINT))
    for (const auto &entry : fs::directory_iterator(path))
      erase(entry.path());
  const bool ok = attributes & FILE_ATTRIBUTE_DIRECTORY
                      ? RemoveDirectoryW(native.c_str())
                      : DeleteFileW(native.c_str());
  if (!ok)
    throw std::runtime_error(
        "Could not remove local data. Close the game or sign-in window first.");
}
} // namespace
void remove_tree(const fs::path &root, const fs::path &target) {
  const fs::path r{platform::native_path(root)},
                 t{platform::native_path(target)};
  auto relative = t.lexically_relative(r);
  if (relative.empty() || relative == "." || relative.has_root_path())
    throw std::runtime_error("Invalid local data removal boundary.");
  for (const auto &part : relative)
    if (part == "..")
      throw std::runtime_error("Local data removal escapes its root.");
  for (auto p = t.parent_path(); p != r; p = p.parent_path())
    if (is_link(p))
      throw std::runtime_error(
          "Cannot remove data through a linked directory.");
  if (is_link(r))
    throw std::runtime_error("Cannot remove data through a linked root.");
  erase(t);
}
void link_directory(const fs::path &link, const fs::path &target) {
  const auto native = platform::native_path(link);
  // A mount point names its target twice: as an NT path (\??\C:\... or
  // \??\UNC\server\share) and as the DOS path shown to users.
  const auto extended = platform::native_path(target);  // \\?\C:\... or \\?\UNC\...
  const std::wstring substitute = L"\\??\\" + extended.substr(4),
                     dos = extended.starts_with(L"\\\\?\\UNC\\") ? L"\\\\" + extended.substr(8) : extended.substr(4);
  if (!CreateDirectoryW(native.c_str(), nullptr))
    throw std::system_error(GetLastError(), std::system_category());
  // REPARSE_DATA_BUFFER's MountPointReparseBuffer (ntifs.h): tag, data length,
  // reserved, then substitute and print name offsets and lengths in bytes.
  const auto sub = substitute.size() * sizeof(wchar_t), print = dos.size() * sizeof(wchar_t);
  std::vector<std::uint8_t> buffer(16 + sub + print + 2 * sizeof(wchar_t));
  auto put16 = [&](std::size_t at, std::size_t value) {
    const auto v = static_cast<std::uint16_t>(value);
    std::memcpy(buffer.data() + at, &v, 2);
  };
  const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
  std::memcpy(buffer.data(), &tag, 4);
  put16(4, buffer.size() - 8);
  put16(8, 0);
  put16(10, sub);
  put16(12, sub + sizeof(wchar_t));
  put16(14, print);
  std::memcpy(buffer.data() + 16, substitute.data(), sub);
  std::memcpy(buffer.data() + 16 + sub + sizeof(wchar_t), dos.data(), print);
  const auto handle = CreateFileW(native.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  DWORD returned = 0;
  const bool ok = handle != INVALID_HANDLE_VALUE &&
                  DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(), DWORD(buffer.size()), nullptr, 0,
                                  &returned, nullptr);
  const auto error = GetLastError();
  if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
  if (!ok) {
    RemoveDirectoryW(native.c_str());
    throw std::system_error(error, std::system_category());
  }
}
void flush_file(const fs::path &file) {
  const auto handle =
      CreateFileW(platform::native_path(file).c_str(), GENERIC_WRITE,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE)
    throw std::system_error(GetLastError(), std::system_category());
  const auto ok = FlushFileBuffers(handle);
  const auto error = GetLastError();
  CloseHandle(handle);
  if (!ok)
    throw std::system_error(error, std::system_category());
}
void flush_directory(const fs::path &) {}
} // namespace venture
