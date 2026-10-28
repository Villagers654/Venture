#include "common/meta/accounts.h"
#include "common/file_ops.h"
#include <algorithm>
#include <set>

namespace venture::meta {
namespace {
bool account_id(std::string_view id) {
  return digits(id, 1, 20) && id.front() != '0' &&
         number(Json(std::string(id))) != 0;
}
} // namespace
bool valid_account_key(std::string_view key) {
  return key.size() == 12 &&
         key.find_first_not_of("0123456789abcdef") == key.npos;
}
const Account *AccountIndex::find(std::string_view key) const {
  auto it = std::ranges::find(accounts, key, &Account::key);
  return it == accounts.end() ? nullptr : &*it;
}
AccountIndex AccountVault::read_index() const {
  AccountIndex result;
  auto file = data_ / "accounts/index.json";
  if (!fs::exists(file))
    return result;
  auto j = paths::read_json(file);
  if (number(j, "version") != 2 || !field(j, "accounts").is_array())
    throw std::runtime_error(
        "Account index is unreadable. Credentials have been retained.");
  std::set<std::string> keys, ids;
  for (const auto &row : j["accounts"]) {
    Account a{str(row, "key"),
              str(row, "accountId"),
              str(row, "addedAt"),
              str(row, "lastUsedAt"),
              field(row, "signedIn") == true,
              field(row, "needsReauth") == true};
    if (!valid_account_key(a.key) || !account_id(a.account_id) ||
        !keys.insert(a.key).second || !ids.insert(a.account_id).second)
      throw std::runtime_error("Account index contains invalid entries. "
                               "Credentials have been retained.");
    result.accounts.push_back(std::move(a));
  }
  result.primary = str(j, "primary");
  result.active = str(j, "active");
  if ((!result.primary.empty() && !result.find(result.primary)) ||
      (!result.active.empty() && !result.find(result.active)))
    throw std::runtime_error(
        "Account index selection is invalid. Credentials have been retained.");
  return result;
}
void AccountVault::write_index(const AccountIndex &index) const {
  Json rows = Json::array();
  for (const auto &a : index.accounts)
    rows.push_back({{"key", a.key},
                    {"accountId", a.account_id},
                    {"signedIn", a.signed_in},
                    {"needsReauth", a.needs_reauth},
                    {"addedAt", a.added_at},
                    {"lastUsedAt", a.last_used_at}});
  paths::atomic_write(data_ / "accounts/index.json",
                      Json{{"version", 2},
                           {"primary", index.primary},
                           {"active", index.active},
                           {"appSharing", false},
                           {"accounts", rows}}
                          .dump(2));
}
void AccountVault::reconcile_locked(AccountIndex &index) const {
  bool changed = false;
  for (auto &a : index.accounts) {
    if (!a.signed_in)
      continue;
    try {
      if (!fs::is_regular_file(data_ / "accounts" / a.key / "session.dpapi")) {
        if (!a.needs_reauth)
          changed = true;
        a.needs_reauth = true;
      }
    } catch (...) {
      if (!a.needs_reauth)
        changed = true;
      a.needs_reauth = true;
    }
  }
  auto root = data_ / "accounts";
  for (const auto &entry : fs::directory_iterator(root)) {
    auto key = paths::utf8(entry.path().filename());
    if (!valid_account_key(key) || is_link(entry.path()) ||
        !entry.is_directory() || index.find(key))
      continue;
    try {
      auto s = SecretStore(data_, key).load();
      if (!s || !account_id(s->account_id) ||
          std::ranges::any_of(index.accounts, [&](const auto &a) {
            return a.account_id == s->account_id;
          }))
        continue;
      if (std::ranges::count(index.accounts, true, &Account::signed_in) >= 4)
        continue;
      index.accounts.push_back(
          {key, s->account_id, s->saved_at, s->saved_at, true, false});
      if (index.primary.empty())
        index.primary = key;
      if (index.active.empty())
        index.active = key;
      changed = true;
    } catch (...) {
    } // Preserve undecryptable/unbound orphan blobs; never adopt them.
  }
  if (changed)
    write_index(index);
}
AccountIndex AccountVault::index() const {
  FileLock lock(data_ / "accounts/.lock");
  return read_index();
}
AccountIndex AccountVault::reconcile() const {
  FileLock lock(data_ / "accounts/.lock");
  auto result = read_index();
  reconcile_locked(result);
  return result;
}
std::optional<Session> AccountVault::load(std::string_view key) const {
  if (!valid_account_key(key))
    throw std::runtime_error("Invalid account key.");
  FileLock lock(data_ / "accounts/.lock");
  auto idx = read_index();
  auto a = idx.find(key);
  if (!a || !a->signed_in || a->needs_reauth)
    return {};
  auto s = SecretStore(data_, std::string(key)).load();
  if (!s || s->account_id != a->account_id)
    throw std::runtime_error("Account credential does not match its slot.");
  return s;
}
std::optional<Session>
AccountVault::load_compatible(std::string_view key) const {
  FileLock lock(data_ / "accounts/.lock");
  auto idx = read_index();
  std::string selected = key.empty() ? idx.active : std::string(key);
  if (selected == "local")
    return {};
  if (selected.empty()) {
    // An explicit sign-out in v2 must not resurrect a restored legacy copy.
    if (!idx.accounts.empty())
      return {};
    return SecretStore(data_).load();
  }
  if (!valid_account_key(selected))
    throw std::runtime_error("Invalid account key.");
  auto account = idx.find(selected);
  if (!account || !account->signed_in)
    return {};
  std::exception_ptr slot_error;
  try {
    if (!account->needs_reauth) {
      auto session = SecretStore(data_, selected).load();
      if (session) {
        if (session->account_id != account->account_id)
          throw std::runtime_error(
              "Account credential does not match its slot.");
        return session;
      }
    }
  } catch (...) {
    slot_error = std::current_exception();
  }
  auto legacy = SecretStore(data_).load();
  if (legacy && legacy->account_id == account->account_id)
    return legacy;
  if (slot_error)
    std::rethrow_exception(slot_error);
  return {};
}
std::string AccountVault::upsert_locked(AccountIndex &idx,
                                        const Session &session) {
  if (!account_id(session.account_id) || session.access_token.empty())
    throw std::runtime_error("Sign-in did not return a stable Meta account.");
  auto it =
      std::ranges::find(idx.accounts, session.account_id, &Account::account_id);
  if ((it == idx.accounts.end() || !it->signed_in) &&
      std::ranges::count(idx.accounts, true, &Account::signed_in) >= 4)
    throw std::runtime_error(
        "Sign out an account before adding another (maximum four).");
  auto key = it == idx.accounts.end() ? random_account_key() : it->key;
  while (it == idx.accounts.end() &&
         (idx.find(key) || fs::exists(data_ / "accounts" / key)))
    key = random_account_key();
  SecretStore store(data_, key);
  store.save(session); // Blob first, index second: interrupted writes become
                       // adoptable orphans.
  auto verify = store.load();
  if (!verify || verify->account_id != session.account_id ||
      verify->access_token != session.access_token)
    throw std::runtime_error("Account credential verification failed. "
                             "Credentials have been retained.");
  auto now = paths::timestamp();
  if (it == idx.accounts.end())
    idx.accounts.push_back({key, session.account_id, now, now, true, false});
  else {
    it->signed_in = true;
    it->needs_reauth = false;
    it->last_used_at = now;
  }
  if (idx.primary.empty())
    idx.primary = key;
  idx.active = key;
  write_index(idx);
  return key;
}
std::string AccountVault::upsert(const Session &s) {
  FileLock lock(data_ / "accounts/.lock");
  auto idx = read_index();
  reconcile_locked(idx);
  return upsert_locked(idx, s);
}
void AccountVault::update_profile(std::string_view key, std::string_view alias,
                                  std::string_view display_name) {
  if (!valid_account_key(key))
    throw std::runtime_error("Invalid account key.");
  FileLock lock(data_ / "accounts/.lock");
  auto idx = read_index();
  auto a = idx.find(key);
  if (!a || !a->signed_in || a->needs_reauth)
    return;
  SecretStore store(data_, std::string(key));
  auto s = store.load();
  if (!s || s->account_id != a->account_id)
    throw std::runtime_error("Account credential does not match its slot.");
  bool changed = false;
  if (s->alias.empty() && !alias.empty()) {
    s->alias = alias;
    changed = true;
  }
  if (s->display_name.empty() && !display_name.empty()) {
    s->display_name = display_name;
    changed = true;
  }
  if (changed)
    store.save(*s);
}
void AccountVault::sign_out(std::string_view key) {
  if (!valid_account_key(key))
    throw std::runtime_error("Invalid account key.");
  FileLock lock(data_ / "accounts/.lock");
  auto idx = read_index();
  auto it = std::ranges::find(idx.accounts, key, &Account::key);
  if (it == idx.accounts.end())
    throw std::runtime_error("Unknown account.");
  SecretStore(data_, std::string(key)).clear();
  it->signed_in = false;
  it->needs_reauth = true;
  if (idx.active == key)
    idx.active.clear();
  write_index(idx);
}
void AccountVault::remove(std::string_view key) {
  if (!valid_account_key(key))
    throw std::runtime_error("Invalid account key.");
  FileLock lock(data_ / "accounts/.lock");
  auto idx = read_index();
  if (!idx.find(key))
    throw std::runtime_error("Unknown account.");
  SecretStore(data_, std::string(key)).clear();
  std::erase_if(idx.accounts, [&](const auto &a) { return a.key == key; });
  if (idx.active == key)
    idx.active.clear();
  if (idx.primary == key)
    idx.primary = idx.accounts.empty() ? "" : idx.accounts.front().key;
  write_index(idx);
}
void AccountVault::set_active(std::string_view key) {
  if (!valid_account_key(key))
    throw std::runtime_error("Invalid account key.");
  FileLock lock(data_ / "accounts/.lock");
  auto idx = read_index();
  auto it = std::ranges::find(idx.accounts, key, &Account::key);
  if (it == idx.accounts.end() || !it->signed_in || it->needs_reauth)
    throw std::runtime_error("Reconnect this account before switching.");
  idx.active = key;
  it->last_used_at = paths::timestamp();
  write_index(idx);
}
void AccountVault::set_primary(std::string_view key) {
  if (!valid_account_key(key))
    throw std::runtime_error("Invalid account key.");
  FileLock lock(data_ / "accounts/.lock");
  auto idx = read_index();
  if (!idx.find(key))
    throw std::runtime_error("Unknown account.");
  idx.primary = key;
  write_index(idx);
}
void AccountVault::set_app_sharing(bool enabled) {
  if (enabled)
    throw std::runtime_error(
        "App Sharing is not supported. Each account must own the game.");
}
Migration
AccountVault::migrate_v1(const std::function<Session(Session)> &resolve) {
  FileLock lock(data_ / "accounts/.lock");
  SecretStore legacy(data_);
  auto session = legacy.load(); // Any failure preserves the original file.
  if (!session)
    return Migration::absent;
  if (!account_id(session->account_id)) {
    if (!resolve)
      return Migration::needs_identity;
    *session = resolve(std::move(*session));
    if (!account_id(session->account_id))
      return Migration::needs_identity;
  }
  auto idx = read_index();
  reconcile_locked(idx);
  auto existing = std::ranges::find(idx.accounts, session->account_id,
                                    &Account::account_id);
  if (existing != idx.accounts.end() &&
      fs::exists(data_ / "accounts" / existing->key / "session.dpapi")) {
    // A restored v1 copy can be older than v2. Verify and adopt the existing
    // bound slot without downgrading its credentials or changing selections.
    auto stored = SecretStore(data_, existing->key).load();
    if (!stored || stored->account_id != session->account_id ||
        stored->access_token.empty())
      throw std::runtime_error("Existing account verification failed. Legacy "
                               "credentials have been retained.");
    existing->signed_in = true;
    existing->needs_reauth = false;
    write_index(idx);
    const auto verified_index = read_index();
    auto verified = SecretStore(data_, existing->key).load();
    if (!verified_index.find(existing->key) || !verified ||
        verified->account_id != stored->account_id ||
        verified->access_token != stored->access_token)
      throw std::runtime_error("Existing account verification failed. Legacy "
                               "credentials have been retained.");
    legacy.clear();
    return Migration::migrated;
  }
  auto key = upsert_locked(idx, *session);
  auto stored_index = read_index();
  auto stored = SecretStore(data_, key).load();
  if (!stored_index.find(key) || !stored ||
      stored->account_id != session->account_id ||
      stored->access_token != session->access_token)
    throw std::runtime_error("Migrated account verification failed. Legacy "
                             "credentials have been retained.");
  legacy.clear();
  return Migration::migrated;
}
} // namespace venture::meta
