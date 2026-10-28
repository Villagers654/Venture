#pragma once
#include "common/secret_store.h"
#include <functional>
#include <vector>

namespace venture::meta {
bool valid_account_key(
    std::string_view key); // exactly 12 lowercase hex; excludes local
struct Account {
  std::string key, account_id, added_at, last_used_at;
  bool signed_in = false, needs_reauth = false;
};
struct AccountIndex {
  std::string primary, active;
  bool app_sharing = false; // owner-accounts only; always false
  std::vector<Account> accounts;
  const Account *find(std::string_view key) const;
};
enum class Migration { absent, migrated, needs_identity };
class AccountVault {
public:
  explicit AccountVault(fs::path data) : data_(std::move(data)) {}
  AccountIndex index() const; // non-secret; never decrypts other accounts
  AccountIndex
  reconcile() const; // launcher startup only: adopts bound orphan blobs
  std::optional<Session>
  load(std::string_view key) const; // decrypts only this slot
  // Transition reader: v2 first, then v1 only for the same signed-in account.
  std::optional<Session> load_compatible(std::string_view key = {}) const;
  std::string upsert(const Session &);
  // Fills this slot's empty alias/display name; never changes the active
  // account or the credential.
  void update_profile(std::string_view key, std::string_view alias,
                      std::string_view display_name);
  void sign_out(std::string_view key); // retains key and saves for reconnect
  void remove(std::string_view key);
  void set_active(std::string_view key);
  void set_primary(std::string_view key);
  void set_app_sharing(bool enabled); // rejects true; MA6 is out of scope
  Migration migrate_v1(const std::function<Session(Session)> &resolve = {});

private:
  fs::path data_;
  AccountIndex read_index() const;
  void write_index(const AccountIndex &) const;
  void reconcile_locked(AccountIndex &) const;
  std::string upsert_locked(AccountIndex &, const Session &);
};
} // namespace venture::meta
