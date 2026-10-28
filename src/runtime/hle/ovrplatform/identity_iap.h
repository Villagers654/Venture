#pragma once
#include <nlohmann/json_fwd.hpp>

#include "hle/ovrplatform/request_args.h"

namespace venture::ovrplatform {
struct ServiceSpec {
  std::string_view name, payload;
};
std::span<const ServiceSpec> identity_iap_services();
void register_identity_iap_endpoints(HttpTransport&);
HttpRequest identity_iap_request(std::string_view, const RequestArgs&);
Model identity_iap_model(std::string_view, const nlohmann::json&);
// Cache only complete, successfully parsed viewer-purchase traversals. Partial
// pages never replace the previous snapshot, and no network reads occur here.
class IapCache {
 public:
  explicit IapCache(std::filesystem::path directory);
  ~IapCache();
  Result read(std::string_view account);
  std::uint64_t generation();
  void page(std::string_view account, std::string_view request_url,
            const nlohmann::json&, std::uint64_t generation);
  void consumed(std::string_view account, std::string_view sku);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace venture::ovrplatform
