#include "libc_b/test.h"
#include <array>
#include <fstream>
#include <thread>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#include "platform/win32/file_mutation.h"
#include <winioctl.h>
#else
#include <unistd.h>
#endif
using namespace venture;
using namespace venture::hle::libc;
using namespace venture::test::libc_b;

TEST_CASE("Android external storage parents support recursive directory creation") {
  Thread thread;
  for (const auto* parent : {"/storage", "/storage/emulated", "/storage/self", "/mnt"}) {
    Stat stat{};
    REQUIRE(path_stat(parent, &stat) == 0);
    CHECK((stat.mode & 0170000) == 0040000);
    CHECK(call("mkdir", {h2g(parent), 0700}) == -1);
    CHECK(*thread.thread.errno_location() == err::eexist);
    const auto fd = fd_open(parent, 0200000, 0);
    REQUIRE(fd >= 0);
    std::vector<Dirent> entries;
    REQUIRE(FdTable::get().file(fd)->entries(entries) == 0);
    CHECK(entries.size() >= 3);
    CHECK(call("close", {std::uint64_t(fd)}) == 0);
  }
  // No host filesystem root or arbitrary sibling is exposed by the parents.
  Stat absent{};
  CHECK(path_stat("/storage/unmounted-volume", &absent) == -err::enoent);
  const auto directory = std::string("/storage/emulated/0/recursive-profile-test");
  REQUIRE(call("mkdir", {h2g(directory.c_str()), 0700}) == 0);
  const auto profile = directory + "/profile.xml";
  const auto fd = fd_open(profile.c_str(), 0100 | 0200 | 1, 0600);
  REQUIRE(fd >= 0);
  CHECK(fd_write(fd, "<profile/>", 10) == 10);
  CHECK(call("close", {std::uint64_t(fd)}) == 0);
  CHECK(call("unlink", {h2g(profile.c_str())}) == 0);
  CHECK(call("rmdir", {h2g(directory.c_str())}) == 0);
}

TEST_CASE("guest completed file writes are visible before close and completion markers") {
  Thread thread;
  const auto metadata = process_config().guest_data_dir() + "/write-order-data.bin";
  const auto marker = process_config().guest_data_dir() + "/write-order-complete.ver";
  std::array<unsigned char, 32768> data{}, observed{};
  for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<unsigned char>(i * 31);
  GuestAddr stream = 0;
  int fd = -1;
  SUBCASE("write has no host-side userspace buffer") {
    fd = static_cast<int>(call("open", {h2g(metadata.c_str()), 0100 | 01000 | 1, 0600}));
    REQUIRE(fd >= 0);
    REQUIRE(call("write", {std::uint64_t(fd), h2g(data.data()), data.size()}) == data.size());
  }
  SUBCASE("guest fwrite also writes through before fflush") {
    stream = static_cast<GuestAddr>(call("fopen", {h2g(metadata.c_str()), h2g("wb")}));
    REQUIRE(stream != 0);
    REQUIRE(call("fwrite", {h2g(data.data()), 32, data.size() / 32, stream}) == data.size() / 32);
  }
  // Another file description sees all acknowledged bytes while the writer is
  // still open. In particular, writing a marker cannot precede a hidden flush.
  const auto reader = call("open", {h2g(metadata.c_str()), 0, 0});
  REQUIRE(reader >= 0);
  REQUIRE(call("read", {std::uint64_t(reader), h2g(observed.data()), observed.size()}) == observed.size());
  CHECK(observed == data);
  CHECK(call("close", {std::uint64_t(reader)}) == 0);
  const auto mark = call("open", {h2g(marker.c_str()), 0100 | 01000 | 1, 0600});
  REQUIRE(mark >= 0);
  CHECK(call("write", {std::uint64_t(mark), h2g("complete"), 8}) == 8);
  CHECK(call("close", {std::uint64_t(mark)}) == 0);
  if (stream) CHECK(call("fclose", {stream}) == 0);
  else CHECK(call("close", {std::uint64_t(fd)}) == 0);
  CHECK(call("unlink", {h2g(metadata.c_str())}) == 0);
  CHECK(call("unlink", {h2g(marker.c_str())}) == 0);
}

TEST_CASE("positioned file IO preserves guest and exported descriptor offsets") {
  Thread thread;
  const auto path = process_config().guest_data_dir() + "/positioned-io.bin";
  const auto moved = path + ".moved";
  const auto fd = fd_open(path.c_str(), 0100 | 01000 | 2, 0600);
  REQUIRE(fd >= 0);
  REQUIRE(fd_write(fd, "abcdef", 6) == 6);
  CHECK(call("access", {h2g(path.c_str()), 0}) == 0);
  CHECK(call("open", {h2g(path.c_str()), 0100 | 0200 | 1, 0600}) == -1);
  CHECK(*thread.thread.errno_location() == err::eexist);
  REQUIRE(fd_seek(fd, 2, 0) == 2);
  REQUIRE(fd_pwrite(fd, "XY", 2, 4) == 2);
  REQUIRE(fd_seek(fd, 0, 1) == 2);
  char bytes[6]{};
  REQUIRE(fd_pread(fd, bytes, 6, 0) == 6);
  CHECK(std::string(bytes, 6) == "abcdXY");
  REQUIRE(fd_seek(fd, 0, 1) == 2);
  REQUIRE(fd_pread(fd, bytes, 6, 100) == 0);
  REQUIRE(fd_seek(fd, 0, 1) == 2);
  // The native libz descriptor shares the sequential position with guest dup.
  const int native = FdTable::get().file(fd)->duplicate_host_fd();
  REQUIRE(native >= 0);
  auto native_read = [&](void* p, int n) {
#ifdef _WIN32
    return _read(native, p, n);
#else
    return ::read(native, p, n);
#endif
  };
  REQUIRE(native_read(bytes, 1) == 1);
  REQUIRE(bytes[0] == 'c');
  REQUIRE(fd_seek(fd, 0, 1) == 3);
  // Native reads bypass File::mutex; positioned IO must not touch their offset.
  std::int64_t write_result = -1;
  std::thread writer([&] { write_result = fd_pwrite(fd, "Z", 1, 5); });
  const auto read_result = native_read(bytes, 1);
  writer.join();
  REQUIRE(read_result == 1);
  REQUIRE(write_result == 1);
  CHECK(bytes[0] == 'd');
  REQUIRE(fd_seek(fd, 0, 1) == 4);
  REQUIRE(call("rename", {h2g(path.c_str()), h2g(moved.c_str())}) == 0);
  REQUIRE(call("unlink", {h2g(moved.c_str())}) == 0);
  CHECK(call("access", {h2g(moved.c_str()), 0}) == -1);
  CHECK(*thread.thread.errno_location() == err::enoent);
  REQUIRE(fd_pread(fd, bytes, 6, 0) == 6);
  CHECK(std::string(bytes, 6) == "abcdXZ");
  CHECK(call("fsync", {std::uint64_t(fd)}) == 0);
#ifdef _WIN32
  CHECK(_close(native) == 0);
#else
  CHECK(::close(native) == 0);
#endif
  CHECK(FdTable::get().close(fd) == 0);
}

TEST_CASE("file path containment rechecks redirected children") {
  Thread thread;
  const auto root = process_config().data_dir;
  const auto inside = root / "containment-inside";
  const auto outside = root.parent_path() / "containment-outside";
  const auto link = root / "containment-link";
  REQUIRE(std::filesystem::create_directory(inside));
  REQUIRE(std::filesystem::create_directory(outside));
  const auto guest = process_config().guest_data_dir() + "/containment-link/missing.bin";
  MappedPath mapped;
  REQUIRE(std::filesystem::create_directory(link));
  CHECK(resolve_path(guest, at_cwd, &mapped) == 0);
  REQUIRE(std::filesystem::remove(link));
  auto make_link = [&](const std::filesystem::path& target) {
#ifdef _WIN32
    // Directory junctions exercise the same redirection without requiring the
    // owner's symlink privilege or Developer Mode setting.
    REQUIRE(std::filesystem::create_directory(link));
    const auto substitute = L"\\??\\" + target.lexically_normal().wstring();
    struct Junction {
      DWORD tag;
      WORD length, reserved, substitute_offset, substitute_length, print_offset, print_length;
      wchar_t paths[4096];
    } data{};
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.substitute_length = WORD(substitute.size() * sizeof(wchar_t));
    data.print_offset = data.substitute_length + sizeof(wchar_t);
    data.length = 8 + data.print_offset + sizeof(wchar_t);
    std::copy(substitute.begin(), substitute.end(), data.paths);
    auto h = CreateFileW(link.wstring().c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    DWORD returned;
    const auto ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, &data, data.length + 8,
                                   nullptr, 0, &returned, nullptr);
    REQUIRE(CloseHandle(h));
    REQUIRE(ok);
#else
    std::filesystem::create_directory_symlink(target, link);
#endif
  };
  make_link(std::filesystem::absolute(outside));
  CHECK(resolve_path(guest, at_cwd, &mapped) == -err::eacces);
  REQUIRE(std::filesystem::remove(link));
  make_link(std::filesystem::absolute(inside));
  CHECK(resolve_path(guest, at_cwd, &mapped) == 0);
  const auto link_guest = process_config().guest_data_dir() + "/containment-link";
  CHECK(call("access", {h2g(link_guest.c_str()), 0}) == 0);
  // A link inside the mount may become dangling between calls.
  REQUIRE(std::filesystem::remove(inside));
  CHECK(call("access", {h2g(link_guest.c_str()), 0}) == -1);
  CHECK(*thread.thread.errno_location() == err::enoent);
  CHECK(call("faccessat", {std::uint64_t(at_cwd), h2g(link_guest.c_str()), 0, 0x100}) == 0);
  REQUIRE(std::filesystem::remove(link));
  REQUIRE(std::filesystem::remove(outside));
}

TEST_CASE("POSIX file mutation unlinks an open file and immediately reuses its name") {
  Thread thread;
  const auto path = process_config().guest_data_dir() + "/posix-unlink-open.bin";
  struct Cleanup {
    std::vector<int> fds;
    std::string path;
    ~Cleanup() {
      for (auto fd : fds) FdTable::get().close(fd);
      call("unlink", {h2g(path.c_str())});
    }
  } cleanup{{}, path};
  const auto original = fd_open(path.c_str(), 0100 | 01000 | 2, 0600);
  REQUIRE(original >= 0); cleanup.fds.push_back(original);
  REQUIRE(fd_write(original, "old", 3) == 3);
  REQUIRE(call("unlink", {h2g(path.c_str())}) == 0);
  CHECK(call("access", {h2g(path.c_str()), 0}) == -1);
  CHECK(*thread.thread.errno_location() == err::enoent);
  const auto replacement = fd_open(path.c_str(), 0100 | 0200 | 2, 0600);
  REQUIRE(replacement >= 0); cleanup.fds.push_back(replacement);
  REQUIRE(fd_write(replacement, "new", 3) == 3);
  char bytes[3]{};
  REQUIRE(fd_pread(original, bytes, 3, 0) == 3);
  CHECK(std::memcmp(bytes, "old", 3) == 0);
  REQUIRE(fd_pread(replacement, bytes, 3, 0) == 3);
  CHECK(std::memcmp(bytes, "new", 3) == 0);
  Stat before{}, after{};
  REQUIRE(call("fstat", {static_cast<GuestAddr>(original), h2g(&before)}) == 0);
  REQUIRE(call("fstat", {static_cast<GuestAddr>(replacement), h2g(&after)}) == 0);
  CHECK(before.ino != after.ino);
}

TEST_CASE("POSIX file mutation replaces an open destination and preserves its aliases and mapping") {
  Thread thread;
  const auto path = process_config().guest_data_dir() + "/posix-rename-open.bin";
  const auto source = path + ".new", alias = path + ".alias", second_alias = path + ".alias2";
  struct Cleanup {
    std::vector<int> fds;
    std::vector<std::string> paths;
    GuestAddr view = 0;
    ~Cleanup() {
      if (view) call("munmap", {view, 4096});
      for (auto fd : fds) FdTable::get().close(fd);
      for (const auto& path : paths) call("unlink", {h2g(path.c_str())});
    }
  } cleanup{{}, {path, source, alias, second_alias}};
  const auto old = fd_open(path.c_str(), 0100 | 01000 | 2, 0600);
  REQUIRE(old >= 0); cleanup.fds.push_back(old);
  REQUIRE(fd_write(old, "old", 3) == 3);
  REQUIRE(call("ftruncate", {static_cast<GuestAddr>(old), 4096}) == 0);
  REQUIRE(call("link", {h2g(path.c_str()), h2g(alias.c_str())}) == 0);
  const auto incoming = fd_open(source.c_str(), 0100 | 01000 | 2, 0600);
  REQUIRE(incoming >= 0); cleanup.fds.push_back(incoming);
  REQUIRE(fd_write(incoming, "new", 3) == 3);
  const auto mapped = call("mmap", {0, 4096, 1, 2, static_cast<GuestAddr>(old), 0});
  REQUIRE(mapped > 0); cleanup.view = static_cast<GuestAddr>(mapped);
  REQUIRE(call("rename", {h2g(source.c_str()), h2g(path.c_str())}) == 0);
  CHECK(call("access", {h2g(source.c_str()), 0}) == -1);
  char bytes[3]{};
  REQUIRE(fd_pread(old, bytes, 3, 0) == 3);
  CHECK(std::memcmp(bytes, "old", 3) == 0);
  CHECK(std::memcmp(g2h(cleanup.view), "old", 3) == 0);
  const auto fresh = fd_open(path.c_str(), 0, 0);
  REQUIRE(fresh >= 0); cleanup.fds.push_back(fresh);
  REQUIRE(fd_pread(fresh, bytes, 3, 0) == 3);
  CHECK(std::memcmp(bytes, "new", 3) == 0);
  const auto linked = fd_open(alias.c_str(), 0, 0);
  REQUIRE(linked >= 0); cleanup.fds.push_back(linked);
  REQUIRE(fd_pread(linked, bytes, 3, 0) == 3);
  CHECK(std::memcmp(bytes, "old", 3) == 0);
  REQUIRE(fd_pwrite(old, "OLD", 3, 0) == 3);
  REQUIRE(fd_pread(linked, bytes, 3, 0) == 3);
  CHECK(std::memcmp(bytes, "OLD", 3) == 0);
  REQUIRE(call("link", {h2g(alias.c_str()), h2g(second_alias.c_str())}) == 0);
  REQUIRE(call("rename", {h2g(alias.c_str()), h2g(second_alias.c_str())}) == 0);
  CHECK(call("access", {h2g(alias.c_str()), 0}) == 0);
  CHECK(call("access", {h2g(second_alias.c_str()), 0}) == 0);
  REQUIRE(fd_pread(fresh, bytes, 3, 0) == 3);
  CHECK(std::memcmp(bytes, "new", 3) == 0);
}

TEST_CASE("POSIX file mutation rejects directory type mismatches without removing names") {
  Thread thread;
  const auto path = process_config().guest_data_dir() + "/posix-rename-type.bin";
  const auto directory = path + ".directory";
  struct Cleanup {
    int fd = -1;
    std::string file, directory;
    ~Cleanup() {
      if (fd >= 0) FdTable::get().close(fd);
      call("unlink", {h2g(file.c_str())});
      call("rmdir", {h2g(directory.c_str())});
    }
  } cleanup{-1, path, directory};
  cleanup.fd = static_cast<int>(fd_open(path.c_str(), 0100 | 01000 | 2, 0600));
  REQUIRE(cleanup.fd >= 0);
  REQUIRE(call("mkdir", {h2g(directory.c_str()), 0700}) == 0);
  CHECK(call("rename", {h2g(path.c_str()), h2g(directory.c_str())}) == -1);
  CHECK(*thread.thread.errno_location() == err::eisdir);
  CHECK(call("rename", {h2g(directory.c_str()), h2g(path.c_str())}) == -1);
  CHECK(*thread.thread.errno_location() == err::enotdir);
  CHECK(call("access", {h2g(path.c_str()), 0}) == 0);
  CHECK(call("access", {h2g(directory.c_str()), 0}) == 0);
}

TEST_CASE("POSIX file mutation removes and replaces readonly files in writable directories") {
  Thread thread;
  const auto path = process_config().guest_data_dir() + "/posix-readonly.bin";
  const auto source = path + ".new";
  struct Cleanup {
    std::vector<int> fds;
    std::string path, source;
    ~Cleanup() {
      for (auto fd : fds) FdTable::get().close(fd);
      for (const auto& p : {path, source}) {
        call("chmod", {h2g(p.c_str()), 0666}); call("unlink", {h2g(p.c_str())});
      }
    }
  } cleanup{{}, path, source};
  auto create = [&](const std::string& p) {
    auto fd = static_cast<int>(fd_open(p.c_str(), 0100 | 01000 | 2, 0600));
    REQUIRE(fd >= 0); cleanup.fds.push_back(fd); return fd;
  };
  const auto old = create(path);
  REQUIRE(fd_write(old, "old", 3) == 3);
  REQUIRE(call("chmod", {h2g(path.c_str()), 0444}) == 0);
  REQUIRE(call("unlink", {h2g(path.c_str())}) == 0);
  const auto target = create(path);
  REQUIRE(fd_write(target, "old", 3) == 3);
  const auto incoming = create(source);
  REQUIRE(fd_write(incoming, "new", 3) == 3);
  REQUIRE(call("chmod", {h2g(path.c_str()), 0444}) == 0);
  REQUIRE(call("chmod", {h2g(source.c_str()), 0444}) == 0);
  REQUIRE(call("rename", {h2g(source.c_str()), h2g(path.c_str())}) == 0);
  const auto fresh = static_cast<int>(fd_open(path.c_str(), 0, 0));
  REQUIRE(fresh >= 0); cleanup.fds.push_back(fresh);
  char bytes[3]{};
  REQUIRE(fd_pread(fresh, bytes, 3, 0) == 3); CHECK(std::memcmp(bytes, "new", 3) == 0);
  REQUIRE(fd_pread(target, bytes, 3, 0) == 3); CHECK(std::memcmp(bytes, "old", 3) == 0);
}

TEST_CASE("POSIX file mutation replaces only empty destination directories") {
  Thread thread;
  const auto from = process_config().guest_data_dir() + "/posix-dir-from";
  const auto to = from + ".to", child = to + "/child", nested = from + "/nested";
  struct Cleanup {
    std::vector<std::string> names;
    ~Cleanup() { for (const auto& p : names) call("rmdir", {h2g(p.c_str())}); }
  } cleanup{{child, nested, from, to}};
  REQUIRE(call("mkdir", {h2g(from.c_str()), 0700}) == 0);
  REQUIRE(call("mkdir", {h2g(to.c_str()), 0700}) == 0);
  REQUIRE(call("mkdir", {h2g(child.c_str()), 0700}) == 0);
  CHECK(call("rename", {h2g(from.c_str()), h2g(to.c_str())}) == -1);
  CHECK(*thread.thread.errno_location() == err::enotempty);
  CHECK(call("access", {h2g(from.c_str()), 0}) == 0);
  CHECK(call("access", {h2g(child.c_str()), 0}) == 0);
  REQUIRE(call("rmdir", {h2g(child.c_str())}) == 0);
  REQUIRE(call("mkdir", {h2g(nested.c_str()), 0700}) == 0);
  CHECK(call("rename", {h2g(from.c_str()), h2g(nested.c_str())}) == -1);
  CHECK(*thread.thread.errno_location() == err::einval);
  CHECK(call("access", {h2g(nested.c_str()), 0}) == 0);
  REQUIRE(call("rmdir", {h2g(nested.c_str())}) == 0);
  REQUIRE(call("rename", {h2g(from.c_str()), h2g(to.c_str())}) == 0);
  CHECK(call("access", {h2g(from.c_str()), 0}) == -1);
  CHECK(*thread.thread.errno_location() == err::enoent);
  CHECK(call("access", {h2g(to.c_str()), 0}) == 0);
}

#ifdef _WIN32
TEST_CASE("POSIX file mutation unsupported filesystem fails without changing names") {
  Thread thread;
  const auto file = process_config().data_dir / "unsupported-mutation.bin";
  const auto target = process_config().data_dir / "unsupported-mutation-target.bin";
  struct Cleanup { std::filesystem::path a, b; ~Cleanup() { std::filesystem::remove(a); std::filesystem::remove(b); } } cleanup{file, target};
  { std::ofstream(file) << "incoming"; std::ofstream(target) << "old"; }
  auto reader = CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, 0, nullptr);
  REQUIRE(reader != INVALID_HANDLE_VALUE);
  struct Close { HANDLE h; ~Close() { CloseHandle(h); } } close{reader};
  const auto unsupported = +[](HANDLE, FILE_INFO_BY_HANDLE_CLASS, LPVOID, DWORD) -> BOOL {
    SetLastError(ERROR_NOT_SUPPORTED); return FALSE;
  };
  CHECK(host::detail::unlink_posix(target.wstring(), unsupported) == -err::enotsup);
  CHECK(host::detail::rename_posix(file.wstring(), target.wstring(), unsupported) == -err::enotsup);
  CHECK(std::filesystem::exists(file)); CHECK(std::filesystem::exists(target));
  char bytes[3]{}; DWORD n = 0;
  REQUIRE(ReadFile(reader, bytes, 3, &n, nullptr)); CHECK(n == 3); CHECK(std::memcmp(bytes, "old", 3) == 0);
  const auto denied = +[](HANDLE, FILE_INFO_BY_HANDLE_CLASS, LPVOID, DWORD) -> BOOL {
    SetLastError(ERROR_ACCESS_DENIED); return FALSE;
  };
  CHECK(host::detail::unlink_posix(target.wstring(), denied) == -err::eacces);
  CHECK(host::detail::rename_posix(file.wstring(), target.wstring(), denied) == -err::eacces);
  CHECK(std::filesystem::exists(file)); CHECK(std::filesystem::exists(target));
  const auto denied_name = file.wstring() + L".denied";
  const auto denied_attributes = +[](LPCWSTR path) -> DWORD {
    if (std::wstring_view(path).ends_with(L".denied")) {
      SetLastError(ERROR_ACCESS_DENIED); return INVALID_FILE_ATTRIBUTES;
    }
    return GetFileAttributesW(path);
  };
  CHECK(host::detail::rename_posix(file.wstring(), denied_name, unsupported, denied_attributes) == -err::eacces);
  CHECK(std::filesystem::exists(file)); CHECK_FALSE(std::filesystem::exists(denied_name));
  const auto absent = file.wstring() + L".absent";
  CHECK(host::detail::rename_posix(file.wstring(), absent, unsupported) == 0);
  CHECK_FALSE(std::filesystem::exists(file)); CHECK(std::filesystem::exists(absent));
  CHECK(std::filesystem::remove(absent));
}

// An interrupted run can leave a recovery sibling, which would shift the
// replacement attempt the next run counts.
static void clear_replace_leftovers() {
  std::error_code ignored;
  std::vector<std::filesystem::path> leftovers;
  for (const auto& entry : std::filesystem::directory_iterator(process_config().data_dir, ignored))
    if (entry.path().filename().wstring().starts_with(L".venture-replace-")) leftovers.push_back(entry.path());
  for (const auto& path : leftovers) {
    std::filesystem::remove(path / "child", ignored);
    std::filesystem::remove(path, ignored);
  }
}
TEST_CASE("POSIX file mutation directory publication failure restores the original inode") {
  Thread thread;
  clear_replace_leftovers();
  const auto source = process_config().data_dir / "directory-rollback-source";
  const auto target = process_config().data_dir / "directory-rollback-target";
  const auto child = target / "child";
  struct Cleanup {
    std::filesystem::path source, target, child;
    ~Cleanup() { std::filesystem::remove(child); std::filesystem::remove(source); std::filesystem::remove(target); }
  } cleanup{source, target, child};
  REQUIRE(std::filesystem::create_directory(source));
  REQUIRE(std::filesystem::create_directory(target));
  REQUIRE(SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_HIDDEN));
  auto identity = [](const std::filesystem::path& path) {
    const auto h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    BY_HANDLE_FILE_INFORMATION info{};
    const bool ok = GetFileInformationByHandle(h, &info); CloseHandle(h);
    REQUIRE(ok); return info;
  };
  const auto before = identity(target);
  static thread_local unsigned publication_failures = 0, moves = 0;
  publication_failures = moves = 0;
  const auto fail_publication = +[](HANDLE h, FILE_INFO_BY_HANDLE_CLASS kind, LPVOID info, DWORD size) -> BOOL {
    if (kind == FileRenameInfoEx) {
      const auto* rename = static_cast<FILE_RENAME_INFO*>(info);
      if (rename->Flags & FILE_RENAME_FLAG_REPLACE_IF_EXISTS) {
        ++publication_failures; SetLastError(ERROR_ACCESS_DENIED); return FALSE;
      }
      ++moves;
    }
    return SetFileInformationByHandle(h, kind, info, size);
  };
  CHECK(host::detail::rename_posix(source.wstring(), target.wstring(), fail_publication) == -err::eacces);
  CHECK(publication_failures == 2); // Initial attempt and retry after moving the victim.
  CHECK(moves == 2); // Move aside, then restore by the same opened handle.
  CHECK(std::filesystem::is_directory(source)); CHECK(std::filesystem::is_directory(target));
  const auto after = identity(target);
  CHECK(after.dwVolumeSerialNumber == before.dwVolumeSerialNumber);
  CHECK(after.nFileIndexHigh == before.nFileIndexHigh); CHECK(after.nFileIndexLow == before.nFileIndexLow);
  CHECK(after.ftCreationTime.dwHighDateTime == before.ftCreationTime.dwHighDateTime);
  CHECK(after.ftCreationTime.dwLowDateTime == before.ftCreationTime.dwLowDateTime);
  CHECK((after.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0);
  { std::ofstream(child) << "must stay"; }
  publication_failures = moves = 0;
  CHECK(host::detail::rename_posix(source.wstring(), target.wstring(), fail_publication) == -err::enotempty);
  CHECK(publication_failures == 1); CHECK(moves == 0);
  CHECK(std::filesystem::exists(child));
}
TEST_CASE("POSIX file mutation child race restores or preserves a predictable recovery sibling") {
  Thread thread;
  const auto source = process_config().data_dir / "directory-race-source";
  const auto target = process_config().data_dir / "directory-race-target";
  static thread_local std::filesystem::path destination, victim;
  static thread_local bool recreate_destination;
  static thread_local unsigned publications;
  destination = target; victim.clear(); recreate_destination = false; publications = 0;
  SUBCASE("restore the same victim when a child appears") {}
  SUBCASE("preserve recovery sibling when destination reappears") { recreate_destination = true; }
  struct Cleanup {
    std::filesystem::path source, target;
    std::filesystem::path& victim;
    ~Cleanup() {
      // A scanner may briefly hold the child open; a destructor must not throw.
      std::error_code ignored;
      for (const auto& p : {victim, target, source}) if (!p.empty()) {
        std::filesystem::remove(p / "child", ignored); std::filesystem::remove(p, ignored);
      }
    }
  } cleanup{source, target, victim};
  for (const auto& p : {source, target}) { std::filesystem::remove(p / "child"); std::filesystem::remove(p); }
  clear_replace_leftovers();
  REQUIRE(std::filesystem::create_directory(source)); REQUIRE(std::filesystem::create_directory(target));
  const auto race = +[](HANDLE h, FILE_INFO_BY_HANDLE_CLASS kind, LPVOID info, DWORD size) -> BOOL {
    if (kind == FileRenameInfoEx) {
      const auto* rename = static_cast<FILE_RENAME_INFO*>(info);
      if (rename->Flags & FILE_RENAME_FLAG_REPLACE_IF_EXISTS) {
        ++publications; SetLastError(ERROR_ACCESS_DENIED); return FALSE;
      }
      const std::filesystem::path name(std::wstring(rename->FileName, rename->FileNameLength / sizeof(wchar_t)));
      if (name.filename().wstring().starts_with(L".venture-replace-")) {
        if (!SetFileInformationByHandle(h, kind, info, size)) return FALSE;
        victim = name;
        { std::ofstream(victim / "child") << "raced child"; }
        if (recreate_destination) std::filesystem::create_directory(destination);
        return TRUE;
      }
    }
    return SetFileInformationByHandle(h, kind, info, size);
  };
  const auto result = host::detail::rename_posix(source.wstring(), target.wstring(), race);
  CHECK(publications == 1); // Child detected after moving aside: no source publication.
  CHECK(std::filesystem::is_directory(source));
  REQUIRE_FALSE(victim.empty());
  CHECK(victim.parent_path() == target.parent_path());
  CHECK(victim.filename().wstring().starts_with(L".venture-replace-"));
  if (recreate_destination) {
    CHECK(result < 0);
    CHECK(std::filesystem::is_directory(target)); CHECK(std::filesystem::is_empty(target));
    CHECK(std::filesystem::exists(victim / "child"));
  } else {
    CHECK(result == -err::enotempty);
    CHECK(std::filesystem::exists(target / "child")); CHECK_FALSE(std::filesystem::exists(victim));
  }
}

#endif
