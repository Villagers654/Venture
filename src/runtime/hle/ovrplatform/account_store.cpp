#include "common/log.h"
#include "common/meta/accounts.h"
#include "hle/ovrplatform/provider.h"

namespace venture::ovrplatform {
namespace {
class VaultStore final : public SecretStore {
  venture::meta::AccountVault vault_{venture::paths::data_dir()};
  std::string key_;

public:
  explicit VaultStore(std::string key) : key_(std::move(key)) {}
  std::filesystem::path platform_directory(std::string_view app) const override {
    if (!venture::meta::valid_account_key(key_) || !valid_id(app)) return {};
    auto root = venture::paths::data_dir();
    auto path = root / "accounts" / key_ / "platform" / std::string(app);
    if (!venture::paths::within(root, path)) throw std::runtime_error("Invalid Platform state path");
    return path;
  }
  Credential load() override {
    if (key_ == "local")
      return {};
    try {
      auto session = vault_.load_compatible(key_);
      if (!session) {
        log::warn("ovrplatform", "Meta session load failed stage=vault-session");
        return {};
      }
      if (session->access_token.empty()) {
        log::warn("ovrplatform", "Meta session load failed stage=empty-token");
        return {};
      }
      return {session->access_token, session->account_id};
    } catch (...) {
      // As in P0's credential reader, report only a local stage; vault errors
      // may contain paths or decrypted fields and must never reach the log.
      log::warn("ovrplatform", "Meta session load failed stage=vault-load");
      throw std::runtime_error("Meta session unavailable");
    }
  }
};
} // namespace
std::shared_ptr<SecretStore> runtime_secret_store(std::string_view key) {
  std::string selected(key);
  if (selected.empty()) {
    try {
      selected = venture::meta::AccountVault(venture::paths::data_dir()).index().active;
    } catch (...) {
      log::warn("ovrplatform", "Meta session load failed stage=vault-index");
      throw std::runtime_error("Meta session unavailable");
    }
  }
  if (!selected.empty() && selected != "local" &&
      !venture::meta::valid_account_key(selected))
    throw std::runtime_error("Invalid account key.");
  return std::make_shared<VaultStore>(std::move(selected));
}
std::shared_ptr<SecretStore> runtime_secret_store() {
  return runtime_secret_store({});
}
} // namespace venture::ovrplatform
