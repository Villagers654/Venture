#include "common/file_ops.h"
#include "common/meta/accounts.h"
#include <doctest/doctest.h>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#endif

using namespace venture;
namespace {
struct Directory {
  fs::path root =
      fs::temp_directory_path() / ("venture-accounts-" + random_account_key());
  Directory() { fs::create_directories(root); }
  ~Directory() {
    try {
      remove_tree(root.parent_path(), root);
    } catch (...) {
    }
  }
};
Session fixture(std::string id = "777777") {
  return {"fixture-secret-" + id, id, "Fixture alias", {}, "test"};
}
} // namespace
TEST_CASE("account keys reject reserved names and traversal") {
  CHECK(meta::valid_account_key("012345abcdef"));
  for (auto key : {"local", "../abcdef123", "ABCDEF123456", "012345abcdef0",
                   "123", "12345678901/"})
    CHECK_FALSE(meta::valid_account_key(key));
}

#ifdef _WIN32
TEST_CASE("Venture reads legacy DPAPI v1 and v2 after data directory migration") {
  Directory d;
  const auto old = d.root / "legacy", current = d.root / "current";
  meta::AccountVault vault(old);
  const auto key = vault.upsert(fixture());
  auto legacy_write = [&](const fs::path& file, int version, const std::string& slot) {
    const auto plain = Json{{"version", version}, {"key", slot},
      {"accessToken", "legacy-fixture-secret"}, {"accountId", "777777"},
      {"alias", "Fixture alias"}, {"source", "test"}}.dump();
    // Independently reproduce the previously released writer's bytes.
    const auto entropy = version == 1 ? std::string("QuestPort.MetaSession.v1") : std::string("QuestPort.MetaSession.v2\0", 25) + slot;
    DATA_BLOB input{DWORD(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
    DATA_BLOB extra{DWORD(entropy.size()), reinterpret_cast<BYTE*>(const_cast<char*>(entropy.data()))}, output{};
    REQUIRE(CryptProtectData(&input, L"legacy fixture", &extra, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output));
    paths::atomic_write(file, std::string_view(reinterpret_cast<char*>(output.pbData), output.cbData));
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
  };
  legacy_write(old / "meta-session.dpapi", 1, "");
  legacy_write(old / "accounts" / key / "session.dpapi", 2, key);
  const auto index = paths::read(old / "accounts/index.json");
  REQUIRE(paths::migrate_data_dir(current, old) == current);
  CHECK(paths::read(current / "accounts/index.json") == index);
  REQUIRE(SecretStore(current).load().has_value());
  CHECK(SecretStore(current).load()->access_token == "legacy-fixture-secret");
  REQUIRE(meta::AccountVault(current).load(key).has_value());
  CHECK(meta::AccountVault(current).load(key)->access_token == "legacy-fixture-secret");
}
TEST_CASE("vault migrates v1, verifies v2, and reattaches the same account") {
  Directory d;
  SecretStore old(d.root);
  old.save(fixture());
  meta::AccountVault vault(d.root);
  REQUIRE(vault.migrate_v1() == meta::Migration::migrated);
  CHECK_FALSE(fs::exists(d.root / "meta-session.dpapi"));
  auto idx = vault.index();
  REQUIRE(idx.accounts.size() == 1);
  CHECK(idx.active == idx.primary);
  CHECK(vault.load(idx.active)->access_token == fixture().access_token);
  auto public_json = paths::read(d.root / "accounts/index.json");
  CHECK(public_json.find("fixture-secret") == std::string::npos);
  CHECK(public_json.find("Fixture alias") == std::string::npos);
  vault.sign_out(idx.active);
  CHECK_FALSE(vault.load(idx.active).has_value());
  CHECK(vault.upsert(fixture()) == idx.active);
  CHECK(vault.migrate_v1() == meta::Migration::absent);
}
TEST_CASE("vault preserves undecryptable legacy and slot-bound blobs") {
  Directory d;
  paths::atomic_write(d.root / "meta-session.dpapi", "undecryptable");
  meta::AccountVault vault(d.root);
  CHECK_THROWS(vault.migrate_v1());
  CHECK(fs::exists(d.root / "meta-session.dpapi"));
  auto a = vault.upsert(fixture()), b = vault.upsert(fixture("888888"));
  auto original = paths::read(d.root / "accounts" / b / "session.dpapi");
  fs::copy_file(d.root / "accounts" / a / "session.dpapi",
                d.root / "accounts" / b / "session.dpapi",
                fs::copy_options::overwrite_existing);
  CHECK_THROWS(vault.load(b));
  CHECK(vault.load(a).has_value());
  paths::atomic_write(d.root / "accounts" / b / "session.dpapi", original);
}
#ifndef _WIN32
TEST_CASE("a copied data root reads its Secret Service credential but never clears it") {
  Directory d;
  const auto origin = d.root / "origin", copy = d.root / "copy";
  const auto key = random_account_key();
  SecretStore(origin, key).save(fixture());
  fs::create_directories(copy / "accounts" / key);
  // A copied root carries every file, including the sealed fallback when no
  // Secret Service is reachable.
  for (auto name : {"session.dpapi", "session.sealed", "session.key"})
    if (fs::exists(origin / "accounts" / key / name))
      fs::copy_file(origin / "accounts" / key / name, copy / "accounts" / key / name);
  auto loaded = SecretStore(copy, key).load();
  REQUIRE(loaded.has_value());
  CHECK(loaded->access_token == "fixture-secret-777777");
  SecretStore(copy, key).clear();
  CHECK_FALSE(SecretStore(copy, key).load().has_value());
  CHECK(SecretStore(origin, key).load().has_value());
  SecretStore(origin, key).clear();
}
TEST_CASE("sign-in protection names where the session was actually stored") {
  Directory d;
  const auto key = random_account_key();
  SecretStore(d.root, key).save(fixture());
  const bool sealed = fs::exists(d.root / "accounts" / key / "session.sealed");
  CHECK(std::string(SecretStore::protection()) ==
        (sealed ? "kept in an encrypted file only your user can read" : "kept in your desktop keyring"));
  if (sealed) {
    auto mode = fs::status(d.root / "accounts" / key / "session.sealed").permissions();
    CHECK((mode & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
  }
  REQUIRE(SecretStore(d.root, key).load().has_value());
  SecretStore(d.root, key).clear();
  CHECK_FALSE(fs::exists(d.root / "accounts" / key / "session.sealed"));
  CHECK_FALSE(SecretStore(d.root, key).load().has_value());
}
#endif
TEST_CASE(
    "vault reconciles orphans and missing blobs without deleting secrets") {
  Directory d;
  meta::AccountVault vault(d.root);
  auto a = random_account_key();
  SecretStore(d.root, a).save(fixture());
  CHECK(vault.reconcile().active == a);
  auto b = random_account_key();
  paths::atomic_write(d.root / "accounts" / b / "session.dpapi",
                      "cannot decrypt");
  CHECK(vault.reconcile().accounts.size() == 1);
  CHECK(fs::exists(d.root / "accounts" / b / "session.dpapi"));
  fs::remove(d.root / "accounts" / a / "session.dpapi");
  CHECK(vault.reconcile().find(a)->needs_reauth);
}
TEST_CASE("vault requires numeric identity and retains offline v1") {
  Directory d;
  auto session = fixture();
  session.account_id.clear();
  SecretStore(d.root).save(session);
  meta::AccountVault vault(d.root);
  CHECK(vault.migrate_v1() == meta::Migration::needs_identity);
  CHECK(fs::exists(d.root / "meta-session.dpapi"));
  CHECK_THROWS(vault.upsert(session));
  CHECK(vault.migrate_v1([](Session s) {
    s.account_id = "777777";
    return s;
  }) == meta::Migration::migrated);
}
TEST_CASE(
    "vault serializes account edits and enforces four signed-in accounts") {
  Directory d;
  meta::AccountVault vault(d.root);
  std::vector<std::jthread> workers;
  for (int i = 0; i < 4; ++i)
    workers.emplace_back(
        [&, i] { vault.upsert(fixture(std::to_string(777777 + i))); });
  workers.clear();
  auto idx = vault.index();
  REQUIRE(idx.accounts.size() == 4);
  CHECK_THROWS(vault.upsert(fixture("999999")));
  vault.sign_out(idx.accounts.front().key);
  CHECK_NOTHROW(vault.upsert(fixture("999999")));
  CHECK_THROWS(vault.set_active("../escape"));
  CHECK_THROWS(vault.set_app_sharing(true));
}
#endif
