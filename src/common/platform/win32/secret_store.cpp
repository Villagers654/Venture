#include "common/secret_store.h"
#include "common/meta/accounts.h"
#include "common/file_ops.h"
#include <windows.h>

#include <wincrypt.h>

namespace venture {
namespace {
// Persisted DPAPI entropy is independent of the product's display name.
constexpr std::string_view legacy_v1_entropy = "QuestPort.MetaSession.v1";
constexpr std::string_view legacy_v2_entropy{"QuestPort.MetaSession.v2\0", 25};
std::string crypt(std::string_view bytes, bool protect, std::string_view key) {
  std::string entropy =
      key.empty()
          ? std::string(legacy_v1_entropy)
          : std::string(legacy_v2_entropy) + std::string(key);
  DATA_BLOB input{DWORD(bytes.size()),
                  reinterpret_cast<BYTE *>(const_cast<char *>(bytes.data()))};
  DATA_BLOB extra{DWORD(entropy.size()),
                  reinterpret_cast<BYTE *>(entropy.data())},
      output{};
  BOOL ok =
      protect
          ? CryptProtectData(&input, L"Venture Meta session", &extra, nullptr,
                             nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)
          : CryptUnprotectData(&input, nullptr, &extra, nullptr, nullptr,
                               CRYPTPROTECT_UI_FORBIDDEN, &output);
  if (!ok)
    throw std::runtime_error(
        "Windows could not unlock the Meta session. Sign in again.");
  std::string result(reinterpret_cast<char *>(output.pbData), output.cbData);
  SecureZeroMemory(output.pbData, output.cbData);
  LocalFree(output.pbData);
  return result;
}
} // namespace
namespace secret_seal {
void save(const fs::path &file, std::string_view context,
          std::string_view plaintext) {
  try {
    paths::atomic_write(file, crypt(plaintext, true, context));
  } catch (const std::exception &) {
    throw std::runtime_error("Could not save the sealed local file.");
  }
}
std::optional<std::string> load(const fs::path &file, std::string_view context) {
  if (!fs::exists(file))
    return {};
  try {
    return crypt(paths::read(file, 1024 * 1024), false, context);
  } catch (const std::exception &) {
    throw std::runtime_error("Could not unlock the sealed local file.");
  }
}
void clear(const fs::path &file) { fs::remove(file); }
} // namespace secret_seal
SecretStore::SecretStore(fs::path directory, std::string key)
    : key_(std::move(key)) {
  if (!meta::valid_account_key(key_))
    throw std::runtime_error("Invalid account key.");
  file_ = std::move(directory) / "accounts" / key_ / "session.dpapi";
  if (is_link(file_.parent_path()) ||
      is_link(file_.parent_path().parent_path()) || is_link(file_))
    throw std::runtime_error("Account credential path is linked.");
}
std::optional<Session> SecretStore::load() const {
  if (!fs::exists(file_))
    return {};
  auto plain = crypt(paths::read(file_, 1024 * 1024), false, key_);
  struct Clear {
    std::string &bytes;
    ~Clear() { SecureZeroMemory(bytes.data(), bytes.size()); }
  } clear{plain};
  auto j = parse_json(plain);
  SecureZeroMemory(plain.data(), plain.size());
  if (number(j, "version") != (key_.empty() ? 1 : 2) ||
      str(j, "accessToken").empty() || (!key_.empty() && str(j, "key") != key_))
    throw std::runtime_error("Invalid Meta session. Sign in again.");
  return Session{str(j, "accessToken"), str(j, "accountId"),
                 str(j, "alias"),       str(j, "savedAt"),
                 str(j, "source"),      str(j, "profileId"),
                 str(j, "displayName")};
}
void SecretStore::save(const Session &s) const {
  auto plain = Json{
      {"version", key_.empty() ? 1 : 2},
      {"key", key_},
      {"accessToken", s.access_token},
      {"accountId", s.account_id},
      {"alias", s.alias},
      {"displayName", s.display_name},
      {"profileId", s.profile_id},
      {"savedAt", paths::timestamp()},
      {"source",
       s.source}}.dump();
  struct Clear {
    std::string &bytes;
    ~Clear() { SecureZeroMemory(bytes.data(), bytes.size()); }
  } clear{plain};
  auto bytes = crypt(plain, true, key_);
  SecureZeroMemory(plain.data(), plain.size());
  paths::atomic_write(file_, bytes);
}
void SecretStore::clear() const { fs::remove(file_); }
const char *SecretStore::protection() {
  return "encrypted for your Windows account";
}
} // namespace venture
