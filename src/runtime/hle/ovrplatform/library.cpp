#include "common/log.h"
#include "common/paths.h"
#include "core/address_space.h"
#include "core/linker.h"
#include "core/process.h"
#include "hle/ovrplatform/asset_service.h"
#include "hle/ovrplatform/broker.h"
#include "hle/ovrplatform/hle.h"
#include "hle/ovrplatform/progression.h"

namespace venture::ovrplatform {
void register_a32_adapters(std::span<const Export> extra);
std::unique_ptr<Session> make_runtime_session(const AppIdentity& app) {
  return make_runtime_session(app, {}, {});
}
std::unique_ptr<Session> make_runtime_session(
    const AppIdentity& app, const std::filesystem::path& install,
    std::shared_ptr<PipeChannel> channel) {
  auto http = meta_http_transport();
  auto provider = channel
      ? brokered_provider(app, process_config().account_key, channel, http)
      : owned_library_provider(
            app, runtime_secret_store(process_config().account_key), http);
  if (!channel)
    log::warn("accounts",
              "CLI credential fallback: the launching account's profile token is "
              "readable by game code. Use the launcher for credential isolation.");
  auto session = make_runtime_session(app, provider);
  if (!install.empty()) {
    auto state = paths::data_dir() / "accounts" / process_config().account_key /
                 "platform" / app.app_id / "language.json";
    register_asset_handlers(
        *session, std::make_shared<AssetService>(
                      app, install, process_config().guest_obb_dir(),
                      asset_ownership_reader(provider, http), channel, state,
                      process_config().obb_dir));
  }
  return session;
}
std::unique_ptr<Session> make_runtime_session(
    const AppIdentity& app, std::shared_ptr<Provider> provider) {
  Memory memory{
      [](std::size_t size) {
        return AddressSpace::get().allocate(size, kProtRead | kProtWrite,
                                            "[ovrplatform]");
      },
      [](Handle address) { AddressSpace::get().free(address); },
  };
  auto session = std::make_unique<Session>(
      app, std::move(provider),
      std::move(memory));
  ProgressionPolicy policy;
  policy.leaderboard_writes = process_config().platform_leaderboard_writes;
  register_progression_handlers(*session, policy);
  return session;
}
std::shared_ptr<HleLibrary> make_library(Session& session, std::span<const Export> extra) {
  const bool a32 = process_config().isa() == GuestIsa::a32;
  if (a32) register_a32_adapters(extra);
  auto library = std::make_shared<HleLibrary>(
      "libovrplatformloader.so",
      std::vector<std::string>{"ovrplatformloader", "ovrplatformloader.so",
                               "libovrplatformloader", "ovrplatform",
                               "libovrplatform.so"});
  // Meta's loader under any file name (Tomahawk's libovr_custom.so in Wizherd)
  // is the same C API; mapped, it fails to find the Quest platform driver.
  library->set_identity({"ovr_PlatformInitializeAndroid", "ovr_PopMessage",
                         "ovr_FreeMessage", "ovr_Message_GetType"});
  for (const auto& entry : exports())
    library->add_function(entry.name,
                          hle::make_thunk(library->soname(), entry.name,
                                          entry.handler, &session));
  for (const auto& entry : extra)
      library->add_function(entry.name,
                            hle::make_thunk(library->soname(), entry.name,
                                            entry.handler, &session));
  return library;
}
void register_library(Session& session, std::span<const Export> extra) {
  Linker::get().register_hle_library(make_library(session, extra));
}
}  // namespace venture::ovrplatform
