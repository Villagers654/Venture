#pragma once
#include <mutex>
#include <set>

#include "common/ipc.h"
#include "common/meta/store.h"
#include "hle/ovrplatform/session.h"

namespace venture::ovrplatform {
struct AssetOwnership {
  Error error;
  std::set<std::string> skus;
};
using AssetOwnershipReader = std::function<AssetOwnership(std::stop_token)>;
// Shared host service for C API and P10's Horizon request/JSON facade. Models
// own their strings; deferred results must be awaited before reporting success.
class AssetService : public std::enable_shared_from_this<AssetService> {
 public:
  using Notify = std::function<void(Model)>;
  AssetService(AppIdentity, std::filesystem::path install,
               std::string guest_root, AssetOwnershipReader,
               std::shared_ptr<PipeChannel> = {},
               std::filesystem::path language_state = {},
               std::filesystem::path obb_view = {});
  Result request(std::string_view operation, const RequestArgs&,
                 std::stop_token, Notify = {});

 private:
  meta::Plan catalog() const;
  std::vector<meta::File> allowed(std::stop_token) const;
  bool installed(const meta::File&) const;
  std::string filepath(const meta::File&) const;
  Model details(const meta::File&, std::stop_token) const;
  Json command(std::string_view, const meta::File&, std::stop_token) const;
  Result download(const meta::File&, std::stop_token, Notify);
  Result await_download(meta::File, std::uint64_t generation, std::stop_token,
                        Notify);
  void select_language(std::string_view);
  AppIdentity app_;
  std::filesystem::path install_, language_state_;
  std::filesystem::path obb_view_;
  std::string guest_root_, language_;
  AssetOwnershipReader ownership_;
  std::shared_ptr<PipeChannel> channel_;
  mutable std::mutex mutex_;
};
AssetOwnershipReader asset_ownership_reader(std::shared_ptr<Provider>,
                                            std::shared_ptr<HttpTransport>);
void register_asset_handlers(Session&, std::shared_ptr<AssetService>);
inline constexpr std::uint32_t kAssetDownloadUpdate =
    0x2fdd0ccd;  // OVR_MessageType.h Notification_AssetFile_DownloadUpdate
}  // namespace venture::ovrplatform
