#pragma once
#include "common/paths.h"
#include <optional>
namespace venture {
struct Session {
  std::string access_token, account_id, alias, saved_at,
      source = "native-launcher";
  std::string profile_id; // optional doc 24112177345042346 metadata; never an
                          // argv value
  std::string display_name; // the account's name as Meta shows it
};
class SecretStore {
public:
  explicit SecretStore(fs::path directory)
      : file_(std::move(directory) / "meta-session.dpapi") {}
  SecretStore(fs::path directory, std::string key);
  std::optional<Session> load() const;
  void save(const Session &session) const;
  void clear() const;
  // How saved sessions are protected, completing "Sign-ins are ...".
  static const char *protection();

private:
  fs::path file_;
  std::string key_;
};
// The file-backed half of SecretStore's own sealing (Windows DPAPI, or on
// POSIX the AES-256-GCM sealed-file fallback it uses with no reachable
// keyring), for callers that are not a Session, e.g. the Meta browser-session
// cookies the native messaging host writes. `context` seals the value to
// this exact file and name, the way SecretStore::save ties it to an account
// key; pass a fixed string unrelated to any account.
namespace secret_seal {
void save(const fs::path &file, std::string_view context,
          std::string_view plaintext);
// Nullopt when `file` does not exist.
std::optional<std::string> load(const fs::path &file, std::string_view context);
void clear(const fs::path &file);
} // namespace secret_seal
} // namespace venture
