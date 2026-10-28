#include "platform/file_activity.h"
#include "common/platform/win32/native_path.h"
#include <windows.h>
#include <vector>
#include <algorithm>
#include <cstddef>
#include "common/paths.h"

namespace venture::platform {
DataWriters data_writers(const std::filesystem::path& root) noexcept {
  try {
    using Query = LONG (NTAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
    auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    if (!query || !std::filesystem::is_directory(root)) return DataWriters::unknown;
    // Access bits are object-specific. Learn the OS file type from our own
    // read-only directory handle before interpreting FILE_WRITE_DATA bits.
    const auto directory = CreateFileW(native_path(root).c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (directory == INVALID_HANDLE_VALUE) return DataWriters::unknown;
    struct Close { HANDLE h; ~Close() { CloseHandle(h); } } close_directory{directory};
    struct Entry { HANDLE handle; ULONG_PTR handles, pointers; ULONG access, type, attributes, reserved; };
    struct Snapshot { ULONG_PTR count, reserved; Entry entries[1]; };
    std::vector<std::byte> bytes(16384);
    LONG status;
    for (;;) {
      ULONG needed = 0;
      status = query(GetCurrentProcess(), 51 /* ProcessHandleInformation */, bytes.data(),
                     static_cast<ULONG>(bytes.size()), &needed);
      if (status >= 0) break;
      if ((static_cast<ULONG>(status) != 0xc0000004 && static_cast<ULONG>(status) != 0xc0000023) ||
          bytes.size() >= 64*1024*1024) return DataWriters::unknown;
      bytes.resize(std::max<std::size_t>(needed, bytes.size()*2));
    }
    const auto& snapshot = *reinterpret_cast<const Snapshot*>(bytes.data());
    if (snapshot.count > (bytes.size()-offsetof(Snapshot, entries))/sizeof(Entry)) return DataWriters::unknown;
    ULONG file_type = 0;
    for (ULONG_PTR i = 0; i < snapshot.count; ++i)
      if (snapshot.entries[i].handle == directory) file_type = snapshot.entries[i].type;
    if (!file_type) return DataWriters::unknown;
    for (ULONG_PTR i = 0; i < snapshot.count; ++i) {
      const auto& entry = snapshot.entries[i];
      if (entry.type != file_type || !(entry.access & (FILE_WRITE_DATA | FILE_APPEND_DATA))) continue;
      SetLastError(ERROR_SUCCESS);
      const auto native_type = GetFileType(entry.handle);
      if (native_type == FILE_TYPE_PIPE || native_type == FILE_TYPE_CHAR) continue;
      if (native_type != FILE_TYPE_DISK &&
          !(native_type == FILE_TYPE_UNKNOWN && GetLastError() == ERROR_SUCCESS)) {
        if (GetLastError() == ERROR_INVALID_HANDLE) continue;
        return DataWriters::unknown;
      }
      HANDLE copied{};
      if (!DuplicateHandle(GetCurrentProcess(), entry.handle, GetCurrentProcess(), &copied, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        if (GetLastError() == ERROR_INVALID_HANDLE) continue; // closed since the snapshot
        return DataWriters::unknown;
      }
      Close close{copied};
      // Recheck the duplicated object before relying on a snapshot handle
      // that might have closed/reused during ordinary application execution.
      if (GetFileType(copied) != native_type) return DataWriters::unknown;
      if (native_type == FILE_TYPE_DISK) {
        FILE_BASIC_INFO info{};
        if (!GetFileInformationByHandleEx(copied, FileBasicInfo, &info, sizeof(info)))
          return DataWriters::unknown;
        // Directory add-file/add-subdirectory rights share the file write bits.
        // They do not represent buffered file contents that exit could lose.
        if (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      }
      std::wstring name(512, L'\0');
      auto length = GetFinalPathNameByHandleW(copied, name.data(), static_cast<DWORD>(name.size()), FILE_NAME_NORMALIZED);
      if (length >= name.size() && length < 32768) {
        name.resize(length+1);
        length = GetFinalPathNameByHandleW(copied, name.data(), static_cast<DWORD>(name.size()), FILE_NAME_NORMALIZED);
      }
      if (!length) {
        const auto error = GetLastError();
        // File-type kernel objects also include non-filesystem devices (for
        // example security handles). Only a successful non-disk classification
        // AND an unsupported filename query exclude them. Disk filename failures
        // and every other uncertainty still fail closed.
        if (native_type == FILE_TYPE_UNKNOWN && (error == ERROR_INVALID_FUNCTION || error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_PARAMETER)) continue;
        return DataWriters::unknown;
      }
      if (length >= name.size()) return DataWriters::unknown;
      name.resize(length);
      if (name.starts_with(L"\\\\?\\UNC\\")) name = L"\\\\" + name.substr(8);
      else if (name.starts_with(L"\\\\?\\")) name.erase(0,4);
      if (paths::within(root, std::filesystem::path(name))) return DataWriters::open;
    }
    return DataWriters::closed;
  } catch (...) { return DataWriters::unknown; }
}
struct FileActivity::Impl {
  HANDLE handle = INVALID_HANDLE_VALUE;
  ~Impl() { if (handle != INVALID_HANDLE_VALUE) FindCloseChangeNotification(handle); }
};
FileActivity::FileActivity(const std::filesystem::path& root) : impl_(std::make_unique<Impl>()) {
  impl_->handle = FindFirstChangeNotificationW(native_path(root).c_str(), TRUE,
      FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
      FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
}
FileActivity::~FileActivity() = default;
FileActivity::Poll FileActivity::poll() {
  if (impl_->handle == INVALID_HANDLE_VALUE) return Poll::failed;
  switch (WaitForSingleObject(impl_->handle, 0)) {
    case WAIT_TIMEOUT: return Poll::quiet;
    case WAIT_OBJECT_0:
      if (FindNextChangeNotification(impl_->handle)) return Poll::changed;
      [[fallthrough]];
    default:
      FindCloseChangeNotification(impl_->handle);
      impl_->handle = INVALID_HANDLE_VALUE;
      return Poll::failed;
  }
}
}  // namespace venture::platform
