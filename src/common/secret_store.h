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
} // namespace venture
