// The Platform HLE served from inside a game's own process: Venture's
// libovrplatformloader.so on Android headsets (docs/design/native-headsets.md,
// 5.3). Every export is a native entry slot (cpu/native_entry.h) whose id
// indexes the export names; the dispatcher runs the same handlers the PC
// runtime registers (hle/ovrplatform/hle.cpp), on a Session whose provider
// asks the Venture app's broker for credentials. Host independent: the x64
// tests drive it through NativeContexts.
#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "core/hle.h"
#include "cpu/native_context.h"
#include "hle/ovrplatform/broker.h"
#include "hle/ovrplatform/hle.h"
#include "hle/ovrplatform/session.h"

namespace venture::ovrplatform {
struct NativePlatformConfig {
  // One request to the Venture app's broker (Binder on Android). It answers
  // "identity" itself and passes the rest to CredentialBroker::handle.
  BrokerCall broker;
  std::shared_ptr<HttpTransport> http;
  // The game's cloud backup folder (its external files dir + "/cloud/data").
  std::filesystem::path cloud_dir;
  // The game's OBB folder (Context.getObbDir), where the Venture app puts its
  // expansion and asset files and their catalog (plan.json); empty: no asset
  // file calls. Downloads are asked of the Venture app through the broker.
  std::filesystem::path obb_dir;
  // Where the chosen language pack is remembered.
  std::filesystem::path language_state;
  // Handlers this host implements itself, over the shared table's (the
  // microphone on the headset's own audio).
  std::span<const Export> extra_exports;
};

// The identity the broker returns for the calling game, or an empty one with
// the broker's error when it refuses or cannot be reached.
struct BrokerIdentity {
  AppIdentity app;
  std::string account;
  ErrorCode error = ErrorCode::none;
};
BrokerIdentity broker_identity(const BrokerCall& broker);

class NativePlatform {
 public:
  // names[id] is the export with that id; names[0] is unused. configure runs
  // once, at the first call, on the calling thread.
  NativePlatform(std::span<const char* const> names, std::function<NativePlatformConfig()> configure);
  ~NativePlatform();
  NativePlatform(const NativePlatform&) = delete;
  NativePlatform& operator=(const NativePlatform&) = delete;

  // Runs export context.x[16]; always completes (VENTURE_NATIVE_EXIT_NONE).
  // An unknown id or an export the HLE does not implement returns 0.
  std::uint32_t dispatch(NativeContext& context) noexcept;
  // After the first call: the session the handlers use.
  Session* session() const { return session_.get(); }
  const BrokerIdentity& identity() const { return identity_; }
  std::size_t implemented() const { return implemented_; }

 private:
  void start();
  std::span<const char* const> names_;
  std::function<NativePlatformConfig()> configure_;
  std::once_flag started_;
  BrokerIdentity identity_;
  std::unique_ptr<Session> session_;
  std::vector<hle::FunctionInfo> functions_;
  std::size_t implemented_ = 0;
};
}  // namespace venture::ovrplatform
