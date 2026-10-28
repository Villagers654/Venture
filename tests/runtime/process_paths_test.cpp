#include "common/file_ops.h"
#include "common/saves_migration.h"
#include "core/process_paths.h"
#include "core/process_runtime.h"
#include <doctest/doctest.h>

using namespace venture;
TEST_CASE("account save roots isolate guest writes from shared host caches") {
  auto saved = process_config();
  auto &c = process_config();
  const auto root =
      fs::temp_directory_path() / ("venture-paths-" + random_account_key());
  c = {};
  c.package = "com.example.game";
  c.account_key = "123456abcdef";
  c.data_dir = account_data_dir(root, c.account_key, c.package);
  c.guest_cache_dir = c.data_dir / "cache";
  c.cache_dir = root / "cache/com.example.game/1";
  c.native_lib_dir = c.cache_dir / "lib/arm64-v8a";
  CHECK(resolve_guest_path(c.guest_data_dir() + "/cache/x") ==
        c.guest_cache_dir / "x");
  CHECK(resolve_guest_path("/data/data/" + c.package + "/cache/x") ==
        c.guest_cache_dir / "x");
  CHECK(host_path_to_guest(paths::utf8(c.guest_cache_dir / "x")) ==
        c.guest_data_dir() + "/cache/x");
  CHECK_FALSE(host_path_to_guest(paths::utf8(c.cache_dir / "java/app.jar"))
                  .has_value());
  CHECK(account_data_dir(root, "abcdef123456", c.package) != c.data_dir);
  CHECK_THROWS(account_data_dir(root, "../escape", c.package));
  CHECK_THROWS(account_data_dir(root, c.account_key, "../escape"));
  process_config() = saved;
}
TEST_CASE("Quest storage canonical paths preserve sdcard loader aliases") {
  const auto saved = process_config();
  auto& c = process_config();
  c = {};
  c.package = "test.game";
  c.data_dir = fs::temp_directory_path() / "venture-path-aliases";
  c.obb_dir = c.data_dir / "external/Android/obb/test.game";
  CHECK(c.guest_obb_dir() == "/storage/emulated/0/Android/obb/test.game");
  CHECK(c.guest_external_files_dir() == "/storage/emulated/0/Android/data/test.game/files");
  for (const auto& tail : {"Android/obb/test.game/main.obb", "Android/data/test.game/files/save", "Download/file"}) {
    CHECK(resolve_guest_path(std::string("/sdcard/") + tail) ==
          resolve_guest_path(std::string("/storage/emulated/0/") + tail));
  }
  CHECK(host_path_to_guest(paths::utf8(c.data_dir / "volumes/data")) == "/data");
  CHECK(host_path_to_guest(paths::utf8(c.data_dir / "volumes/system")) == "/system");
  CHECK(host_path_to_guest(paths::utf8(c.obb_dir / "main.obb")) == c.guest_obb_dir() + "/main.obb");
  process_config() = saved;
}
TEST_CASE("save migration preserves hard links and journals conflicts without "
          "overwriting") {
  auto root = fs::temp_directory_path() / ("venture-saves-" + random_account_key());
  fs::create_directories(root / "data/com.example.game");
  paths::atomic_write(root / "installed.obb", "fixture");
  fs::create_hard_link(root / "installed.obb",
                       root / "data/com.example.game/main.obb");
  auto first = migrate_saves(root, "123456abcdef");
  CHECK(first.moved == 1);
  auto target = account_data_dir(root, "123456abcdef", "com.example.game");
  CHECK(fs::equivalent(root / "installed.obb", target / "main.obb"));
  CHECK(migrate_saves(root, "123456abcdef").moved == 0);
  paths::atomic_write(root / "data/com.example.game/save", "legacy");
  auto conflict = migrate_saves(root, "123456abcdef");
  CHECK(conflict.conflicts == 1);
  CHECK(fs::exists(root / "data/com.example.game/save"));
  CHECK(fs::exists(target / "main.obb"));
  CHECK(paths::read(root / "data/users/migration.json").find("conflict") !=
        std::string::npos);
  remove_tree(root.parent_path(), root);
}
TEST_CASE("first account adopts local saves with an explicit journal entry") {
  auto root = fs::temp_directory_path() / ("venture-local-" + random_account_key());
  paths::atomic_write(root / "data/com.example.game/save", "fixture");
  CHECK(migrate_saves(root, {}).moved == 1);
  CHECK(migrate_saves(root, "123456abcdef", true).moved == 1);
  CHECK(fs::exists(account_data_dir(root, "123456abcdef", "com.example.game") /
                   "save"));
  CHECK(paths::read(root / "data/users/migration.json").find("adopted-local") !=
        std::string::npos);
  remove_tree(root.parent_path(), root);
}
