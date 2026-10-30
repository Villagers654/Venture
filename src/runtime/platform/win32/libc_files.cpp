#include "hle/libc/vfs.h"
#include "core/os_quit.h"
#include "platform/win32/file_mutation.h"
#include "common/platform/win32/native_path.h"
#include <windows.h>
#include <bcrypt.h>
#include <io.h>
#include <fcntl.h>
#include <algorithm>
#include <array>
#include <format>
#include "common/log.h"
#include "common/paths.h"
#include <cstring>

namespace venture::hle::libc::host {
namespace {
#ifndef FILE_RENAME_FLAG_IGNORE_READONLY_ATTRIBUTE
// Published Windows SDK flag; not yet named by the pinned mingw headers.
constexpr DWORD FILE_RENAME_FLAG_IGNORE_READONLY_ATTRIBUTE = 0x00000040;
#endif
int error(DWORD e = GetLastError()) {
  switch (e) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
      return -err::enoent;
    case ERROR_ACCESS_DENIED:
      return -err::eacces;
    case ERROR_INVALID_HANDLE:
      return -err::ebadf;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
      return -err::eexist;
    case ERROR_DISK_FULL:
      return -err::enospc;
    case ERROR_DIR_NOT_EMPTY:
      return -err::enotempty;
    case ERROR_DIRECTORY:
      return -err::enotdir;
    case ERROR_NOT_SAME_DEVICE:
      return -err::exdev;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
      return -err::eacces;
    case ERROR_INVALID_PARAMETER:
    case ERROR_INVALID_NAME:
      return -err::einval;
    case ERROR_FILENAME_EXCED_RANGE:
      return -err::enametoolong;
    case ERROR_TOO_MANY_OPEN_FILES:
      return -err::emfile;
    case ERROR_NOT_ENOUGH_MEMORY:
      return -err::enomem;
    default:
      return -err::eio;
  }
}
std::wstring native(const std::filesystem::path& p) {
  return venture::platform::native_path(p);
}
// Unsupported filesystems must fail explicitly, preserving the original names.
// Legacy delete/replace can return success with incompatible open-file semantics.
bool unsupported_information(DWORD e) {
  return e == ERROR_INVALID_PARAMETER || e == ERROR_NOT_SUPPORTED ||
         e == ERROR_CALL_NOT_IMPLEMENTED || e == ERROR_INVALID_FUNCTION;
}
struct MutationHandle {
  HANDLE value;
  ~MutationHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
MutationHandle mutation_handle(const std::wstring& name, DWORD extra_access = 0) {
  return {CreateFileW(name.c_str(), DELETE | extra_access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
}
bool rename_handle(HANDLE file, const std::wstring& destination, DWORD flags, detail::FileInformation set_info) {
  if (destination.size() > 32767) { SetLastError(ERROR_FILENAME_EXCED_RANGE); return false; }
  const auto bytes = destination.size() * sizeof(wchar_t);
  std::vector<std::byte> buffer(sizeof(FILE_RENAME_INFO) + bytes);
  auto* rename = std::construct_at(reinterpret_cast<FILE_RENAME_INFO*>(buffer.data()));
  rename->Flags = flags; rename->FileNameLength = static_cast<DWORD>(bytes);
  std::memcpy(rename->FileName, destination.data(), bytes);
  return set_info(file, FileRenameInfoEx, rename, static_cast<DWORD>(buffer.size()));
}
int directory_empty(HANDLE directory) {
  alignas(FILE_ID_BOTH_DIR_INFO) std::array<std::byte, 4096> buffer{};
  auto info = FileIdBothDirectoryRestartInfo;
  for (;;) {
    if (!GetFileInformationByHandleEx(directory, info, buffer.data(), static_cast<DWORD>(buffer.size()))) {
      const auto e = GetLastError();
      return e == ERROR_NO_MORE_FILES ? 0 : error(e);
    }
    for (auto* entry = reinterpret_cast<FILE_ID_BOTH_DIR_INFO*>(buffer.data());;) {
      const std::wstring_view name(entry->FileName, entry->FileNameLength / sizeof(wchar_t));
      if (name != L"." && name != L"..") return -err::enotempty;
      if (!entry->NextEntryOffset) break;
      entry = reinterpret_cast<FILE_ID_BOTH_DIR_INFO*>(reinterpret_cast<std::byte*>(entry) + entry->NextEntryOffset);
    }
    info = FileIdBothDirectoryInfo;
  }
}
int unlink_posix_impl(const std::wstring& name, detail::FileInformation set_info) {
  const auto attributes = GetFileAttributesW(name.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
      !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return -err::eisdir;
  auto file = mutation_handle(name);
  if (file.value == INVALID_HANDLE_VALUE) return error();
  // Closing this DELETE handle removes the name immediately; other handles
  // retain the old stream and a subsequent open can create a distinct file.
  FILE_DISPOSITION_INFO_EX disposition{};
  disposition.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
                      FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE;
  if (set_info(file.value, FileDispositionInfoEx, &disposition, sizeof(disposition))) return 0;
  const auto e = GetLastError();
  if (!unsupported_information(e)) return error(e);
  return -err::enotsup;
}
int rename_posix_impl(const std::wstring& source, const std::wstring& destination,
                      detail::FileInformation set_info, detail::FileAttributes attributes) {
  const auto from = attributes(source.c_str());
  const auto to = attributes(destination.c_str());
  const auto attribute_error = to == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
  const bool absent = to == INVALID_FILE_ATTRIBUTES &&
      (attribute_error == ERROR_FILE_NOT_FOUND || attribute_error == ERROR_PATH_NOT_FOUND);
  const bool directories = from != INVALID_FILE_ATTRIBUTES && to != INVALID_FILE_ATTRIBUTES &&
      (from & FILE_ATTRIBUTE_DIRECTORY) && (to & FILE_ATTRIBUTE_DIRECTORY) &&
      !(from & FILE_ATTRIBUTE_REPARSE_POINT) && !(to & FILE_ATTRIBUTE_REPARSE_POINT);
  if (from != INVALID_FILE_ATTRIBUTES && to != INVALID_FILE_ATTRIBUTES) {
    const bool from_directory = (from & FILE_ATTRIBUTE_DIRECTORY) && !(from & FILE_ATTRIBUTE_REPARSE_POINT);
    const bool to_directory = (to & FILE_ATTRIBUTE_DIRECTORY) && !(to & FILE_ATTRIBUTE_REPARSE_POINT);
    if (from_directory != to_directory) return from_directory ? -err::enotdir : -err::eisdir;
  }
  auto file = mutation_handle(source);
  if (file.value == INVALID_HANDLE_VALUE) return error();
  // POSIX rename of two names for the same inode is a no-op, preserving both
  // hardlinks. Attribute queries are optional; the OS remains authoritative.
  if (to != INVALID_FILE_ATTRIBUTES) {
    MutationHandle target{CreateFileW(destination.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    BY_HANDLE_FILE_INFORMATION a{}, b{};
    if (target.value != INVALID_HANDLE_VALUE && GetFileInformationByHandle(file.value, &a) &&
        GetFileInformationByHandle(target.value, &b) && a.dwVolumeSerialNumber == b.dwVolumeSerialNumber &&
        a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow) return 0;
  }
  if (directories) {
    const auto src = std::filesystem::path(source).lexically_normal().wstring();
    const auto dst = std::filesystem::path(destination).lexically_normal().wstring();
    if (dst.size() > src.size() && !_wcsnicmp(src.c_str(), dst.c_str(), src.size()) &&
        (dst[src.size()] == L'\\' || dst[src.size()] == L'/')) return -err::einval;
  }
  constexpr DWORD replacement_flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS |
      FILE_RENAME_FLAG_POSIX_SEMANTICS | FILE_RENAME_FLAG_IGNORE_READONLY_ATTRIBUTE;
  if (rename_handle(file.value, destination, replacement_flags, set_info)) return 0;
  auto e = GetLastError();
  if (directories && e == ERROR_ACCESS_DENIED) {
    auto target = mutation_handle(destination, FILE_LIST_DIRECTORY);
    if (target.value == INVALID_HANDLE_VALUE) return error();
    BY_HANDLE_FILE_INFORMATION a{}, b{};
    if (!GetFileInformationByHandle(file.value, &a) || !GetFileInformationByHandle(target.value, &b)) return error();
    if (!(b.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return -err::enotdir;
    if (a.dwVolumeSerialNumber != b.dwVolumeSerialNumber) return -err::exdev;
    if (const auto result = directory_empty(target.value)) return result;
    auto target_path = std::filesystem::path(destination);
    while (target_path.filename().empty() && target_path.has_relative_path()) target_path = target_path.parent_path();
    constexpr DWORD move_flags = FILE_RENAME_FLAG_POSIX_SEMANTICS | FILE_RENAME_FLAG_IGNORE_READONLY_ATTRIBUTE;
    bool moved = false;
    std::filesystem::path aside;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
      aside = target_path.parent_path() / std::format(L".venture-replace-{}", attempt);
      if (rename_handle(target.value, aside.wstring(), move_flags, set_info)) { moved = true; break; }
      e = GetLastError();
      if (e != ERROR_ALREADY_EXISTS && e != ERROR_FILE_EXISTS) return error(e);
    }
    if (!moved) return -err::eexist;
    auto restore = [&](int failure) {
      if (!rename_handle(target.value, destination, move_flags, set_info)) {
        const auto rollback = GetLastError();
        log::error("libc", "Directory rename rollback failed (error {}): destination={} recovery_sibling={}",
                   rollback, paths::utf8(target_path.filename()), paths::utf8(aside.filename()));
        return error(rollback);
      }
      return failure;
    };
    // Windows has no atomic directory replacement: a short missing-name window remains.
    if (const auto result = directory_empty(target.value)) return restore(result);
    if (!rename_handle(file.value, destination, replacement_flags, set_info)) return restore(error());
    FILE_DISPOSITION_INFO_EX disposition{};
    disposition.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
                        FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE;
    if (!set_info(target.value, FileDispositionInfoEx, &disposition, sizeof(disposition)))
      log::warn("libc", "Directory replacement completed; temporary directory cleanup failed (error {})", GetLastError());
    return 0; // Publication committed; cleanup failure must not report a failed rename.
  }
  if (unsupported_information(e)) {
    if (to != INVALID_FILE_ATTRIBUTES) return -err::enotsup;
    if (!absent) return error(attribute_error);
    // No replace flag: a target appearing after the attribute check is preserved.
    return MoveFileExW(source.c_str(), destination.c_str(), 0) ? 0 : error();
  }
  return error(e);
}

Timespec time(FILETIME f) {
  ULARGE_INTEGER u;
  u.LowPart = f.dwLowDateTime;
  u.HighPart = f.dwHighDateTime;
  auto n = static_cast<std::int64_t>(u.QuadPart) - 116444736000000000ll;
  return {n / 10000000, (n % 10000000) * 100};
}
int fill(HANDLE h, Stat* s) {
  BY_HANDLE_FILE_INFORMATION i;
  if (!GetFileInformationByHandle(h, &i)) return error();
  *s = {};
  s->dev = i.dwVolumeSerialNumber;
  s->ino = (std::uint64_t(i.nFileIndexHigh) << 32) | i.nFileIndexLow;
  s->mode = (i.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0040777 : 0100666;
  if (!(i.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
      (i.dwFileAttributes & FILE_ATTRIBUTE_READONLY)) s->mode &= ~0222u;
  s->size = (std::uint64_t(i.nFileSizeHigh) << 32) | i.nFileSizeLow;
  s->blocks = (s->size + 511) / 512;
  s->nlink = i.nNumberOfLinks;
  s->atim = time(i.ftLastAccessTime);
  s->mtim = time(i.ftLastWriteTime);
  s->ctim = time(i.ftCreationTime);
  return 0;
}
int change_times(HANDLE h, const Timespec* times) {
  FILETIME values[2];
  const FILETIME* pointers[2] = {&values[0], &values[1]};
  for (int i = 0; i < 2; ++i) {
    auto t = times ? times[i] : Timespec{0, 1073741823};
    if (t.nsec == 1073741822) {
      pointers[i] = nullptr;
      continue;
    }
    if (t.nsec == 1073741823)
      GetSystemTimeAsFileTime(&values[i]);
    else {
      if (t.nsec < 0 || t.nsec >= 1000000000 || t.sec < -11644473600ll ||
          t.sec > INT64_MAX / 10000000 - 11644473600ll)
        return -err::einval;
      auto n = static_cast<std::uint64_t>((t.sec + 11644473600ll) * 10000000 + t.nsec / 100);
      values[i] = {static_cast<DWORD>(n), static_cast<DWORD>(n >> 32)};
    }
  }
  return SetFileTime(h, nullptr, pointers[0], pointers[1]) ? 0 : error();
}
int change_mode(HANDLE h, std::uint32_t mode) {
  // Win32 FILE_ATTRIBUTE_READONLY affects files, not directories (Microsoft
  // File Attribute Constants). It cannot express owner/group-specific access,
  // execute permission or set-id bits. Reject those rather than claim success.
  static std::mutex mutation_mutex;
  std::lock_guard lock(mutation_mutex);
  FILE_BASIC_INFO info{};
  int r = 0;
  if (!GetFileInformationByHandleEx(h, FileBasicInfo, &info, sizeof(info)))
    r = error();
  else if (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
    if (mode != 0777) r = -err::enotsup;
  } else if (mode != 0666 && mode != 0444)
    r = -err::enotsup;
  else {
    auto attrs = info.FileAttributes & ~(FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_NORMAL);
    if (mode == 0444) attrs |= FILE_ATTRIBUTE_READONLY;
    info = {}; // zero timestamps leave them unchanged
    info.FileAttributes = attrs ? attrs : FILE_ATTRIBUTE_NORMAL;
    if (!SetFileInformationByHandle(h, FileBasicInfo, &info, sizeof(info))) r = error();
  }
  return r;
}
class DiskFile final : public File {
 public:
  bool readiness_notifications() const override { return true; }
  DiskFile(HANDLE h, std::filesystem::path p, bool directory, bool readonly)
      : handle_(h), host_path_(std::move(p)), directory_(directory), readonly_(readonly) {}
  ~DiskFile() override { CloseHandle(handle_); }
  int duplicate_host_fd() override {
    std::lock_guard lock(mutex);
    if (directory_) return -err::eisdir;
    HANDLE copy;
    if (!DuplicateHandle(GetCurrentProcess(), handle_, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS)) return error();
    int mode = (flags.load() & 3) == 0 ? _O_RDONLY : (flags.load() & 3) == 1 ? _O_WRONLY : _O_RDWR;
    int fd = _open_osfhandle(reinterpret_cast<std::intptr_t>(copy), mode | _O_BINARY);
    if (fd < 0) { CloseHandle(copy); return -err::emfile; }
    native_fd_exported_ = true;
    return fd;
  }
  std::int64_t io(HANDLE handle, bool write, void* p, std::uint64_t n, OVERLAPPED* offset = nullptr) {
    os_quit::DiskWrite writing(write);
    if (directory_) return -err::eisdir;
    DWORD count = 0;
    auto size = static_cast<DWORD>(std::min<std::uint64_t>(n, 0x7ffff000));
    BOOL ok = write ? WriteFile(handle, p, size, &count, offset) : ReadFile(handle, p, size, &count, offset);
    if (!ok) return GetLastError() == ERROR_HANDLE_EOF ? 0 : error();
    return count;
  }
  std::int64_t read(void* p, std::uint64_t n) override {
    std::lock_guard lock(mutex);
    return io(handle_, false, p, n);
  }
  std::int64_t write(const void* p, std::uint64_t n) override {
    std::lock_guard lock(mutex);
    if ((flags.load() & 02000) && !SetFilePointerEx(handle_, {}, nullptr, FILE_END)) return error();
    return io(handle_, true, const_cast<void*>(p), n);
  }
  std::int64_t positioned(bool write, void* p, std::uint64_t n, std::int64_t off) {
    if (directory_) return -err::eisdir;
    {
      std::lock_guard lock(mutex);
      // Before a CRT descriptor is exported, every sequential position user
      // takes this mutex. Save/restore on the existing synchronous handle avoids
      // opening (and closing) the file again for each pread/pwrite. Once libz or
      // another native user can bypass the mutex, use an independent position.
      if (!native_fd_exported_) {
        LARGE_INTEGER saved;
        if (!SetFilePointerEx(handle_, {}, &saved, FILE_CURRENT)) return error();
        OVERLAPPED offset{};
        offset.Offset = static_cast<DWORD>(off);
        offset.OffsetHigh = static_cast<DWORD>(std::uint64_t(off) >> 32);
        // A synchronous handle honors OVERLAPPED offsets but still updates its
        // file pointer, so restoration remains necessary under the mutex.
        auto r = io(handle_, write, p, n, &offset);
        if (!SetFilePointerEx(handle_, saved, nullptr, FILE_BEGIN) && r >= 0)
          return error();
        return r;
      }
    }
    // ReOpenFile creates an independent file position, preserving the shared
    // sequential position of guest dup() and CRT descriptors used by libz.
    auto handle = ReOpenFile(handle_, write ? GENERIC_WRITE : GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0);
    if (handle == INVALID_HANDLE_VALUE) return error();
    OVERLAPPED offset{};
    offset.Offset = static_cast<DWORD>(off);
    offset.OffsetHigh = static_cast<DWORD>(std::uint64_t(off) >> 32);
    auto r = io(handle, write, p, n, &offset);
    CloseHandle(handle);
    return r;
  }
  std::int64_t pread(void* p, std::uint64_t n, std::int64_t off) override { return positioned(false, p, n, off); }
  std::int64_t pwrite(const void* p, std::uint64_t n, std::int64_t off) override { return positioned(true, const_cast<void*>(p), n, off); }
  std::int64_t seek(std::int64_t off, int whence) override {
    if (whence < 0 || whence > 2) return -err::einval;
    std::lock_guard lock(mutex);
    LARGE_INTEGER distance, position; distance.QuadPart = off;
    return SetFilePointerEx(handle_, distance, &position, whence) ? position.QuadPart : error();
  }
  int stat(Stat* s) override { return fill(handle_, s); }
  int truncate(std::int64_t n) override {
    if (n < 0) return -err::einval;
    std::lock_guard lock(mutex);
    FILE_END_OF_FILE_INFO end; end.EndOfFile.QuadPart = n;
    return SetFileInformationByHandle(handle_, FileEndOfFileInfo, &end, sizeof(end)) ? 0 : error();
  }
  int sync() override { return FlushFileBuffers(handle_) ? 0 : error(); }
  int set_times(const Timespec* t) override { return change_times(handle_, t); }
  int chmod(std::uint32_t mode) override {
    if (readonly_) return -err::erofs;
    // fchmod works on O_RDONLY descriptors and follows the open file even if
    // its name is replaced. ReOpenFile obtains attribute access by identity.
    auto h = ReOpenFile(handle_, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        FILE_FLAG_BACKUP_SEMANTICS);
    if (h == INVALID_HANDLE_VALUE) return error();
    const auto r = change_mode(h, mode);
    CloseHandle(h);
    return r;
  }
  std::int64_t available() override {
    Stat s;
    int r = stat(&s);
    if (r) return r;
    auto position = seek(0, 1);
    return position < 0 ? position : std::max<std::int64_t>(0, s.size - position);
  }
  int entries(std::vector<Dirent>& out) override {
    if (!directory_) return -err::enotdir;
    for (auto name : {".", ".."}) {
      Dirent d{};
      d.type = 4;
      d.reclen = sizeof(d);
      std::strcpy(d.name, name);
      out.push_back(d);
    }
    std::error_code ec;
    for (auto& entry : std::filesystem::directory_iterator(
             std::filesystem::path{native(host_path_)}, ec)) {
      Dirent d{};
      const auto n = paths::guest_name(paths::utf8(entry.path().filename()));
      if (n.size() > 255) continue;
      std::memcpy(d.name, n.data(), n.size());
      Stat s;
      if (host::stat(entry.path(), &s, false)) continue;
      d.ino = s.ino;
      d.type = (s.mode & 0170000) == 0040000 ? 4 : 8;
      d.reclen = sizeof(d);
      out.push_back(d);
    }
    return ec ? -err::eio : 0;
  }

 private:
  HANDLE handle_;
  std::filesystem::path host_path_;
  bool directory_, readonly_;
  bool native_fd_exported_ = false; // protected by File::mutex
};
}  // namespace
namespace detail {
int unlink_posix(const std::wstring& name, FileInformation fn) { return unlink_posix_impl(name, fn); }
int rename_posix(const std::wstring& from, const std::wstring& to, FileInformation fn, FileAttributes attrs) {
  return rename_posix_impl(from, to, fn, attrs);
}
}
bool unredirected_path(const std::filesystem::path& root, const std::filesystem::path& path) {
  auto a = root.begin(), b = path.begin();
  for (; a != root.end(); ++a, ++b)
    if (b == path.end() || *a != *b) return false;
  // The configured mount itself is trusted, including any junctions above it.
  // Inspect every child on each call, without caching: a guest can replace a
  // directory with a symlink between calls. Reparse paths use the canonical
  // containment check in resolve_path instead.
  auto child = root;
  for (; b != path.end(); ++b) {
    child /= *b;
    const auto name = native(child);
    const auto attrs = GetFileAttributesW(name.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return GetLastError() == ERROR_FILE_NOT_FOUND;
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) return false;
    auto next = b; ++next;
    if (next != path.end() && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) return false;
  }
  return true;
}
int exists(const std::filesystem::path& path, bool follow) {
  const auto name = native(path);
  WIN32_FILE_ATTRIBUTE_DATA attrs{};
  if (!GetFileAttributesExW(name.c_str(), GetFileExInfoStandard, &attrs)) return error();
  // Attributes describe the link, not its target. Follow reparse points just
  // as stat/access do, including failure for a dangling link.
  if (follow && (attrs.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
    Stat s;
    return host::stat(path, &s, true);
  }
  return 0;
}
std::shared_ptr<File> import_readonly_handle(std::intptr_t value) {
  if(value==0 || value==-1)return {};
  auto h=ReOpenFile(reinterpret_cast<HANDLE>(value),GENERIC_READ,
                    FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,0);
  if(h==INVALID_HANDLE_VALUE)return {};
  Stat info;
  if(fill(h,&info) || (info.mode&0170000)!=0100000){CloseHandle(h);return {};}
  return std::make_shared<DiskFile>(h,std::filesystem::path{},false,true);
}
std::shared_ptr<File> open(const MappedPath& p, int flags, std::uint32_t, int* e) {
  auto name = native(p.host);
  DWORD attributes = GetFileAttributesW(name.c_str());
  bool dir = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
  if ((flags & 0200000) && !dir) {
    *e = attributes == INVALID_FILE_ATTRIBUTES ? error() : -err::enotdir;
    return {};
  }
  if (dir && (flags & 3)) {
    *e = -err::eisdir;
    return {};
  }
  if ((flags & 0400000) && attributes != INVALID_FILE_ATTRIBUTES &&
      (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
    *e = -err::eloop;
    return {};
  }
  DWORD access = (flags & 3) == 0   ? GENERIC_READ
                 : (flags & 3) == 1 ? GENERIC_WRITE
                                    : GENERIC_READ | GENERIC_WRITE;
  DWORD disposition = (flags & 0100)    ? ((flags & 0200)    ? CREATE_NEW
                                           : (flags & 01000) ? CREATE_ALWAYS
                                                             : OPEN_ALWAYS)
                      : (flags & 01000) ? TRUNCATE_EXISTING
                                        : OPEN_EXISTING;
  HANDLE h =
      CreateFileW(name.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, disposition, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    *e = error();
    return {};
  }
  return std::make_shared<DiskFile>(h, p.host, dir, p.readonly);
}
std::int64_t write_standard(int fd, const void* data, std::uint64_t size) {
  DWORD written = 0;
  auto handle = GetStdHandle(fd == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
  if (!WriteFile(handle, data, static_cast<DWORD>(std::min<std::uint64_t>(size, 0x7ffff000)), &written, nullptr)) return error();
  return written;
}
int stat(const std::filesystem::path& p, Stat* s, bool follow) {
  auto n = native(p);
  HANDLE h = CreateFileW(
      n.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | (follow ? 0 : FILE_FLAG_OPEN_REPARSE_POINT), nullptr);
  if (h == INVALID_HANDLE_VALUE) return error();
  int r = fill(h, s);
  if (!r && !follow) {
    auto attrs = GetFileAttributesW(n.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT))
      s->mode = 0120777;
  }
  CloseHandle(h);
  return r;
}
int mutate(int op, const MappedPath& a, const MappedPath* b, std::uint32_t) {
  auto n = native(a.host);
  auto m = b ? native(b->host) : L"";
  switch (op) {
    case 1:
      return CreateDirectoryW(n.c_str(), nullptr) ? 0 : error();
    case 2:
      return detail::unlink_posix(n);
    case 3:
      return RemoveDirectoryW(n.c_str()) ? 0 : error();
    case 4:
      return detail::rename_posix(n, m);
    case 5:
      return CreateHardLinkW(m.c_str(), n.c_str(), nullptr) ? 0 : error();
    case 6: {
      auto relative = std::filesystem::relative(a.host, b->host.parent_path()).wstring();
      auto attrs = GetFileAttributesW(n.c_str());
      DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE |
                    ((attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
                         ? SYMBOLIC_LINK_FLAG_DIRECTORY
                         : 0);
      return CreateSymbolicLinkW(m.c_str(), relative.c_str(), flags) ? 0 : error();
    }
    default:
      return -err::enosys;
  }
}
int set_times(const std::filesystem::path& p, const Timespec* t, bool follow) {
  auto n = native(p);
  HANDLE h = CreateFileW(
      n.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | (follow ? 0 : FILE_FLAG_OPEN_REPARSE_POINT), nullptr);
  if (h == INVALID_HANDLE_VALUE) return error();
  int r = change_times(h, t);
  CloseHandle(h);
  return r;
}
int chmod(const std::filesystem::path& p, std::uint32_t mode) {
  auto n = native(p);
  HANDLE h = CreateFileW(n.c_str(), FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (h == INVALID_HANDLE_VALUE) return error();
  const auto r = change_mode(h, mode);
  CloseHandle(h);
  return r;
}
int memory_lock(GuestAddr p, std::uint64_t n, bool lock) {
  return (lock ? VirtualLock(g2h(p), n) : VirtualUnlock(g2h(p), n)) ? 0 : error();
}
int random(void* p, std::uint64_t n) {
  auto* bytes = static_cast<unsigned char*>(p);
  while (n) {
    auto count = static_cast<ULONG>(std::min<std::uint64_t>(n, ULONG_MAX));
    if (BCryptGenRandom(nullptr, bytes, count, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
      return -err::eio;
    bytes += count;
    n -= count;
  }
  return 0;
}
void yield() { SwitchToThread(); }
std::int64_t cpu_time(bool thread) {
  FILETIME a, b, k, u;
  BOOL ok = thread ? GetThreadTimes(GetCurrentThread(), &a, &b, &k, &u)
                   : GetProcessTimes(GetCurrentProcess(), &a, &b, &k, &u);
  if (!ok) return -err::eio;
  return ((std::uint64_t(k.dwHighDateTime) << 32 | k.dwLowDateTime) +
          (std::uint64_t(u.dwHighDateTime) << 32 | u.dwLowDateTime)) *
         100;
}
}  // namespace venture::hle::libc::host
