#include "common/download/manager.h"
#include "common/file_ops.h"
#include "common/library/library.h"
#include "common/meta/accounts.h"
#include <doctest/doctest.h>
#include <thread>

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
TEST_CASE("LaunchOtherApp stays bound to its originating account after account switching") {
  meta::AccountIndex idx;
  idx.accounts = {{"123456abcdef", "42", {}, {}, true, false},
                  {"abcdef123456", "84", {}, {}, true, false}};
  const auto caller = idx.accounts[0].key;
  idx.active = idx.accounts[1].key;
  CHECK(library::launch_choice(idx, {caller}, caller).account_key == caller);
  CHECK(library::launch_choice(idx, {idx.active}, caller).account_key == caller);
  idx.accounts[0].signed_in = false;
  CHECK(library::launch_choice(idx, {caller}, caller).status == library::LaunchStatus::reconnect);
}

#ifdef _WIN32
TEST_CASE("downloads migrate to the primary and filter and cancel by account") {
  Directory d;
  meta::AccountVault vault(d.root);
  auto primary = vault.upsert(fixture());
  auto second = vault.upsert(fixture("888888"));
  paths::atomic_write(
      d.root / "downloads.json",
      Json{{"version", 1},
           {"jobs",
            Json::array({Json{{"appId", "123456"},
                              {"name", "Fixture"},
                              {"root", paths::utf8(d.root / "games")}}})}}
          .dump());
  download::Manager manager(d.root, [] {});
  manager.set_account(primary);
  REQUIRE(manager.snapshot().size() == 1);
  CHECK(manager.snapshot()[0].account == primary);
  manager.set_account(second);
  CHECK(manager.snapshot().empty());
  manager.cancel_account(primary);
  CHECK(manager.snapshot(primary)[0].state == download::State::paused);
  auto json = paths::read_json(d.root / "downloads.json");
  CHECK(number(json, "version") == 2);
  CHECK(str(json["jobs"][0], "account") == primary);
  CHECK(json.dump().find("fixture-secret") == std::string::npos);
}
#endif
