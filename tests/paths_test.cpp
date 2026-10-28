#include "common/environment.h"
#include "common/file_ops.h"
#include <doctest/doctest.h>
#include <atomic>
#include <exception>
#include <fstream>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

using namespace venture;
TEST_CASE("Venture migration renames atomically and preserves existing defaults") {
  const auto root = fs::temp_directory_path() / ("venture-migrate-" + random_account_key());
  struct Cleanup { fs::path root; ~Cleanup() { remove_tree(root.parent_path(), root); } } cleanup{root};
  const auto old = root / "legacy", current = root / "current";
  CHECK(paths::migrate_data_dir(current, old) == current);
  paths::atomic_write(old / "accounts/index.json", "unchanged index");
  CHECK(paths::migrate_data_dir(current, old) == current);
  CHECK_FALSE(fs::exists(old));
  CHECK(paths::read(current / "accounts/index.json") == "unchanged index");
  paths::atomic_write(old / "keep", "older profile");
  CHECK(paths::migrate_data_dir(current, old) == current);
  CHECK(paths::read(old / "keep") == "older profile");
}
TEST_CASE("Venture migration failure keeps legacy data and retries") {
  const auto root = fs::temp_directory_path() / ("venture-retry-" + random_account_key());
  struct Cleanup { fs::path root; ~Cleanup() { remove_tree(root.parent_path(), root); } } cleanup{root};
  const auto old = root / "legacy", current = root / "missing-parent/current";
  paths::atomic_write(old / "save", "game save");
  CHECK(paths::migrate_data_dir(current, old) == old);
  CHECK(paths::read(old / "save") == "game save");
  fs::create_directory(current.parent_path());
  CHECK(paths::migrate_data_dir(current, old) == current);
  CHECK(paths::read(current / "save") == "game save");
  CHECK_FALSE(fs::exists(old));
}
TEST_CASE("OBB file publication and cleanup support long per-account paths") {
  const auto root = fs::temp_directory_path() / ("venture-long-" + random_account_key());
  struct Cleanup { fs::path root; ~Cleanup() { remove_tree(root.parent_path(), root); } } cleanup{root};
  const auto view = root / std::string(80, 'a') / std::string(80, 'b');
  fs::create_directories(view);
  const auto target = view / (std::string(100, 'c') + ".bundle");
  REQUIRE(target.native().size() > 260);
  paths::atomic_write(root / "source", "downloaded bytes");
  paths::replace_file(root / "source", target);
  CHECK_FALSE(is_link(target));
  CHECK(paths::read(target) == "downloaded bytes");
  // Publication must open its temporary file through the extended namespace,
  // not only support a long destination in replace_file().
  paths::atomic_write(target, "replacement generation");
  CHECK(paths::read(target) == "replacement generation");
  remove_tree(root, target);
  CHECK_FALSE(fs::exists(target));
}
TEST_CASE("local readers permit atomic publication and see a complete generation") {
  const auto root = fs::temp_directory_path() / ("venture-read-" + random_account_key());
  struct Cleanup {
    fs::path root;
    ~Cleanup() { remove_tree(root.parent_path(), root); }
  } cleanup{root};
  const auto file = root / "manifest";
  const std::string first(4 * 1024 * 1024, 'A'), second(first.size(), 'B');
  paths::atomic_write(file, first);
  std::atomic<bool> ready = false, done = false;
  std::exception_ptr reader_error, writer_error;
  std::uint64_t reads = 0;
  std::jthread reader([&] {
    try {
      do {
        ready = true;
        if (!paths::within(root, file))
          throw std::runtime_error("Published manifest escaped its root");
        auto text = paths::read(file);
        if (text != first && text != second)
          throw std::runtime_error("Reader saw a partial generation");
        ++reads;
      } while (!done);
    } catch (...) { reader_error = std::current_exception(); }
  });
  while (!ready) std::this_thread::yield();
  try {
    for (int i = 0; i < 32; ++i) paths::atomic_write(file, i % 2 ? first : second);
  } catch (...) { writer_error = std::current_exception(); }
  done = true;
  reader.join();
  std::string reader_message, writer_message;
  if (reader_error) {
    try { std::rethrow_exception(reader_error); }
    catch (const std::exception& e) { reader_message = e.what(); }
  }
  if (writer_error) {
    try { std::rethrow_exception(writer_error); }
    catch (const std::exception& e) { writer_message = e.what(); }
  }
  INFO(reader_message, " / ", writer_message);
  CHECK_FALSE(reader_error);
  CHECK_FALSE(writer_error);
  CHECK(reads > 0);
  CHECK(paths::read(file) == first);
  CHECK(paths::read(root / "manifest", first.size()) == first);
  CHECK_THROWS(paths::read(file, first.size() - 1));
  paths::atomic_write(file, "");
  CHECK(paths::read(file).empty());
  CHECK_THROWS(paths::read(root / "absent"));
}

TEST_CASE("file replacement preserves an open reader and failed publication") {
  const auto root = fs::temp_directory_path() / ("venture-replace-" + random_account_key());
  struct Cleanup { fs::path root; ~Cleanup() { remove_tree(root.parent_path(), root); } } cleanup{root};
  paths::atomic_write(root / "target", "old bytes");
#ifdef _WIN32
  // Match paths::read: Windows readers opt into replacement with SHARE_DELETE.
  struct Reader {
    HANDLE handle;
    ~Reader() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
  } reader{CreateFileW((root / "target").c_str(), GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
  REQUIRE(reader.handle != INVALID_HANDLE_VALUE);
#else
  std::ifstream reader(root / "target", std::ios::binary);
  REQUIRE(reader.is_open());
#endif
  paths::atomic_write(root / "source", "new bytes");
  paths::replace_file(root / "source", root / "target");
  CHECK(paths::read(root / "target") == "new bytes");
#ifdef _WIN32
  char bytes[9]{};
  DWORD count = 0;
  REQUIRE(ReadFile(reader.handle, bytes, sizeof(bytes), &count, nullptr));
  CHECK(std::string(bytes, count) == "old bytes");
  // A reader denying deletion must leave the old destination and source intact.
  paths::atomic_write(root / "source", "next bytes");
  {
    std::ifstream denied(root / "target", std::ios::binary);
    REQUIRE(denied.is_open());
    CHECK_THROWS(paths::replace_file(root / "source", root / "target"));
    CHECK(paths::read(root / "target") == "new bytes");
    CHECK(paths::read(root / "source") == "next bytes");
  }
  paths::replace_file(root / "source", root / "target");
  CHECK(paths::read(root / "target") == "next bytes");
  paths::atomic_write(root / "target", "new bytes");
#else
  CHECK(std::string(std::istreambuf_iterator<char>(reader), {}) == "old bytes");
#endif
  CHECK_FALSE(fs::exists(root / "source"));
  CHECK_THROWS(paths::replace_file(root / "missing", root / "target"));
  CHECK(paths::read(root / "target") == "new bytes");
}
