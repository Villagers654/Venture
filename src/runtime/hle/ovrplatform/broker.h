#pragma once
#include "common/ipc.h"
#include "hle/ovrplatform/provider.h"
#include <chrono>
#include <mutex>

namespace venture::ovrplatform {
// Launcher-side only: profile credentials never appear in the wire packet.
class CredentialBroker {
public:
  CredentialBroker(AppIdentity, std::string account_key,
                   std::shared_ptr<SecretStore>,
                   std::shared_ptr<HttpTransport>);
  Error prepare(std::stop_token = {});
  venture::Json handle(const venture::Json &);

private:
  AppIdentity app_;
  std::string key_;
  std::shared_ptr<SecretStore> store_;
  std::shared_ptr<HttpTransport> http_;
  Credential profile_;
  std::shared_ptr<Provider> provider_;
  Result identity_;
  std::chrono::steady_clock::time_point last_refresh_{};
  std::mutex mutex_;
  Error valid();
  Error prepare_locked(std::stop_token);
  venture::Json packet() const;
};
// The child provider has no vault/store reference and cannot load profile
// tokens.
std::shared_ptr<Provider> brokered_provider(AppIdentity,
                                            std::string account_key,
                                            std::shared_ptr<PipeChannel>,
                                            std::shared_ptr<HttpTransport>);
} // namespace venture::ovrplatform
