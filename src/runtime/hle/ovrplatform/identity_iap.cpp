#include "hle/ovrplatform/identity_iap.h"

#include <algorithm>
#include <charconv>
#include <mutex>
#include <nlohmann/json.hpp>

#include "common/log.h"
#include "common/paths.h"

namespace venture::ovrplatform {
namespace {
using Json = nlohmann::json;
constexpr ServiceSpec services[] = {
    {"ovr_UserAgeCategory_Get", "UserAccountAgeCategory"},
    {"ovr_User_GetOrgScopedID", "OrgScopedID"},
    {"ovr_User_Get", "User"},
    {"ovr_IAP_GetProductsBySKU", "ProductArray"},
    {"ovr_IAP_GetViewerPurchases", "PurchaseArray"},
    {"ovr_IAP_GetNextProductArrayPage", "ProductArray"},
    {"ovr_IAP_GetNextPurchaseArrayPage", "PurchaseArray"},
    {"ovr_IAP_GetViewerPurchasesDurableCache", "PurchaseArray"},
    {"ovr_IAP_ConsumePurchase", ""}};
// Checkout intentionally uses Session's immediate unsupported completion; a
// payment-UI error must not wait behind the HTTP worker queue.
// Signed Meta Platform 1.205 builders 0x154fe0/0x155380, fields at
// 0x8997f0/0x8999c0. POST consume (0x158c50) has an empty body, query sku.
constexpr auto product_fields =
    "sku,current_offer{description,name,price{currency,amount_in_hundredths,"
    "formatted}},"
    "type,short_description,icon_url,cover_url,content_rating{age_rating_text,"
    "descriptors,"
    "interactive_elements,rating_definition_uri,age_rating_image_uri},billing_"
    "plans{"
    "trial_offers{price{currency,amount_in_hundredths,formatted},trial_term,"
    "trial_type,"
    "max_term_count},paid_offer{price{currency,amount_in_hundredths,formatted},"
    "subscription_term}}";
constexpr auto purchase_fields =
    "id,grant_time,expiration_time,item{sku,type},developer_payload,reporting_"
    "id";
[[noreturn]] void malformed() {
  throw std::invalid_argument("Invalid Platform response");
}
std::string text(const Json& j, std::string_view key, bool required = false) {
  if (!j.is_object()) malformed();
  auto it = j.find(key);
  if (it == j.end() || it->is_null()) {
    if (required) malformed();
    return {};
  }
  if (!it->is_string()) malformed();
  auto value = it->get<std::string>();
  if (value.find('\0') != value.npos || (required && value.empty()))
    malformed();
  return value;
}
std::uint64_t number(const Json& j, std::string_view key,
                     bool required = false) {
  auto it = j.find(key);
  if (it == j.end() || it->is_null()) {
    if (required) malformed();
    return 0;
  }
  if (it->is_number_unsigned()) return it->get<std::uint64_t>();
  if (it->is_number_integer() && it->get<std::int64_t>() >= 0)
    return static_cast<std::uint64_t>(it->get<std::int64_t>());
  malformed();
}
std::uint64_t id(const Json& j, std::string_view key) {
  auto value = text(j, key, true);
  if (!valid_id(value)) malformed();
  std::uint64_t result = 0;
  std::from_chars(value.data(), value.data() + value.size(), result);
  return result;
}
std::int32_t enumeration(std::string_view value,
                         std::initializer_list<std::string_view> names) {
  std::int32_t i = 0;
  for (auto name : names) {
    if (name == value) return i;
    ++i;
  }
  return 0;  // SDK string-to-enum conversions preserve future values as
             // Unknown.
}
std::int32_t product_type(const Json& j) {
  return enumeration(text(j, "type", true),
                     {"UNKNOWN", "DURABLE", "CONSUMABLE", "SUBSCRIPTION"});
}
std::int32_t term(const Json& j, std::string_view key) {
  return enumeration(text(j, key, true),
                     {"UNKNOWN", "WEEKLY", "BIWEEKLY", "MONTHLY", "QUARTERLY",
                      "SEMIANNUAL", "ANNUAL", "BIANNUAL"});
}
void nested(Model& m, std::string field, Model child) {
  m.fields.emplace(std::move(field),
                   std::make_shared<const Model>(std::move(child)));
}
Model price(const Json& j) {
  auto amount = number(j, "amount_in_hundredths", true);
  if (amount > UINT32_MAX) malformed();
  return {"Price",
          {{"AmountInHundredths", std::uint32_t(amount)},
           {"Currency", text(j, "currency", true)},
           {"Formatted", text(j, "formatted", true)}}};
}
const Json& elements(const Json& j) {
  // Graph nested connections use {data:[...]}; accept the array representation
  // too, as used by SDK response mappers, without synthesizing missing data.
  const auto& a = j.is_object() ? j.at("data") : j;
  if (!a.is_array() || a.size() > 16384) malformed();
  return a;
}
Model trial(const Json& j) {
  auto count = number(j, "max_term_count", true);
  if (count > INT32_MAX) malformed();
  Model m{
      "TrialOffer",
      {{"MaxTermCount", std::int32_t(count)},
       {"TrialTerm", term(j, "trial_term")},
       {"TrialType", enumeration(text(j, "trial_type", true),
                                 {"UNKNOWN", "INTRO_OFFER", "FREE_TRIAL"})}}};
  nested(m, "Price", price(j.at("price")));
  return m;
}
Model paid(const Json& j) {
  Model m{"PaidOffer", {{"SubscriptionTerm", term(j, "subscription_term")}}};
  nested(m, "Price", price(j.at("price")));
  return m;
}
Model billing(const Json& j) {
  Model m{"BillingPlan"};
  nested(m, "PaidOffer", paid(j.at("paid_offer")));
  if (j.contains("trial_offers") && !j["trial_offers"].is_null()) {
    Model a{"TrialOfferArray"};
    for (const auto& v : elements(j["trial_offers"]))
      a.elements.push_back(trial(v));
    nested(m, "TrialOffers", std::move(a));
  }
  return m;
}
Model rating(const Json& j) {
  Model m{"ContentRating",
          {{"AgeRatingImageUri", text(j, "age_rating_image_uri")},
           {"AgeRatingText", text(j, "age_rating_text")},
           {"RatingDefinitionUri", text(j, "rating_definition_uri")}}};
  for (auto [json_key, field] :
       {std::pair{"descriptors", "Descriptors"},
        std::pair{"interactive_elements", "InteractiveElements"}}) {
    std::vector<std::string> strings;
    if (j.contains(json_key) && !j[json_key].is_null())
      for (const auto& v : elements(j[json_key])) {
        if (!v.is_string()) malformed();
        auto s = v.get<std::string>();
        if (s.find('\0') != s.npos) malformed();
        strings.push_back(std::move(s));
      }
    m.fields.emplace(field, std::move(strings));
  }
  return m;
}
Model product(const Json& j) {
  const auto& offer = j.at("current_offer");
  auto p = price(offer.at("price"));
  Model m{"Product",
          {{"SKU", text(j, "sku", true)},
           {"Type", product_type(j)},
           {"Description", text(offer, "description")},
           {"Name", text(offer, "name", true)},
           {"FormattedPrice", std::get<std::string>(p.fields.at("Formatted"))},
           {"ShortDescription", text(j, "short_description")},
           {"IconUrl", text(j, "icon_url")},
           {"CoverUrl", text(j, "cover_url")}}};
  nested(m, "Price", std::move(p));
  if (j.contains("content_rating") && !j["content_rating"].is_null())
    nested(m, "ContentRating", rating(j["content_rating"]));
  if (j.contains("billing_plans") && !j["billing_plans"].is_null()) {
    Model a{"BillingPlanArray"};
    for (const auto& v : elements(j["billing_plans"]))
      a.elements.push_back(billing(v));
    nested(m, "BillingPlans", std::move(a));
  }
  return m;
}
Model purchase(const Json& j) {
  auto str_id = text(j, "id", true);
  std::uint64_t numeric = 0;
  // PurchaseStrID is authoritative; future opaque IDs must not be truncated.
  auto [end, ec] =
      std::from_chars(str_id.data(), str_id.data() + str_id.size(), numeric);
  if (ec != std::errc{} || end != str_id.data() + str_id.size()) numeric = 0;
  const auto& item = j.at("item");
  return {"Purchase",
          {{"PurchaseID", numeric},
           {"PurchaseStrID", std::move(str_id)},
           {"GrantTime", number(j, "grant_time", true)},
           {"ExpirationTime", number(j, "expiration_time")},
           {"DeveloperPayload", text(j, "developer_payload")},
           {"ReportingId", text(j, "reporting_id")},
           {"SKU", text(item, "sku", true)},
           {"Type", product_type(item)}}};
}
std::string next_url(const Json& j) {
  if (!j.contains("paging") || j["paging"].is_null()) return {};
  auto next = text(j["paging"], "next");
  if (!next.empty() && !valid_next_page_url(next)) malformed();
  return next;
}
Model array(const Json& j, bool products) {
  Model a{products ? "ProductArray" : "PurchaseArray",
          {{"NextUrl", next_url(j)}}};
  for (const auto& v : elements(j))
    a.elements.push_back(products ? product(v) : purchase(v));
  return a;
}
template <class T>
const T& arg(const RequestArgs& a, std::size_t i) {
  if (i >= a.values.size())
    throw std::invalid_argument("Invalid Platform argument");
  auto p = std::get_if<T>(&a.values[i]);
  if (!p) throw std::invalid_argument("Invalid Platform argument");
  return *p;
}
bool sku_valid(std::string_view s) {
  return !s.empty() && s.size() <= 64 * 1024 &&
         s.find_first_of(",\r\n") == s.npos && s.find('\0') == s.npos;
}
}  // namespace
std::span<const ServiceSpec> identity_iap_services() { return services; }
void register_identity_iap_endpoints(HttpTransport& http) {
  http.register_endpoint({"identity-age", HttpMethod::get, "/me", {"fields"}});
  http.register_endpoint(
      {"identity-user", HttpMethod::get, "/{id}", {"fields"}});
  http.register_endpoint({"iap-products",
                          HttpMethod::get,
                          "/application/available_purchases",
                          {"fields", "skus"}});
  http.register_endpoint({"iap-purchases",
                          HttpMethod::get,
                          "/application/viewer_purchases",
                          {"fields"}});
  http.register_endpoint({"iap-consume",
                          HttpMethod::post,
                          "/application/consume_entitlement",
                          {"sku"}});
}
HttpRequest identity_iap_request(std::string_view name,
                                 const RequestArgs& args) {
  if (args.error) throw std::invalid_argument("Invalid Platform argument");
  if (name == "ovr_UserAgeCategory_Get")
    return {"identity-age", {{"fields", "age_category"}}};
  if (name == "ovr_User_GetOrgScopedID" || name == "ovr_User_Get") {
    auto user_id = arg<std::uint64_t>(args, 0);
    if (!user_id) throw std::invalid_argument("Invalid Platform ID");
    HttpRequest r{"identity-user"};
    r.id = std::to_string(user_id);
    if (name == "ovr_User_GetOrgScopedID")
      r.parameters.emplace_back("fields", "org_scoped_id");
    return r;
  }
  if (name == "ovr_IAP_GetProductsBySKU") {
    const auto& skus = arg<std::vector<std::string>>(args, 0);
    auto count = arg<std::int32_t>(args, 1);
    if (skus.size() > 4096 || count < std::int32_t(skus.size()) || count > 4096)
      throw std::invalid_argument("Invalid Platform SKU count");
    std::string joined;
    for (const auto& sku : skus) {
      if (!sku_valid(sku)) throw std::invalid_argument("Invalid Platform SKU");
      if (!joined.empty()) joined += ',';
      joined += sku;
    }
    HttpRequest r{"iap-products", {{"fields", product_fields}}};
    if (!joined.empty()) r.parameters.emplace_back("skus", std::move(joined));
    return r;
  }
  if (name == "ovr_IAP_GetViewerPurchases")
    return {"iap-purchases", {{"fields", purchase_fields}}};
  if (name == "ovr_IAP_ConsumePurchase") {
    auto sku = arg<std::string>(args, 0);
    if (!sku_valid(sku)) throw std::invalid_argument("Invalid Platform SKU");
    return {"iap-consume", {{"sku", std::move(sku)}}};
  }
  if (name == "ovr_IAP_GetNextProductArrayPage" ||
      name == "ovr_IAP_GetNextPurchaseArrayPage") {
    bool products = name == "ovr_IAP_GetNextProductArrayPage";
    const auto& page = arg<PageSnapshot>(args, 0);
    if (page.kind != (products ? "ProductArray" : "PurchaseArray") ||
        page.next_url.empty() || !valid_next_page_url(page.next_url))
      throw std::invalid_argument("Invalid Platform page");
    HttpRequest r{products ? "iap-products" : "iap-purchases"};
    r.next_url = page.next_url;
    return r;
  }
  throw std::invalid_argument("Unknown Platform service");
}
Model identity_iap_model(std::string_view name, const Json& j) {
  if (name == "ovr_UserAgeCategory_Get") {
    auto value = text(j, "age_category", true);
    return {
        "UserAccountAgeCategory",
        {{"AgeCategory", enumeration(value, {"UNKNOWN", "CH", "TN", "AD"})}}};
  }
  if (name == "ovr_User_GetOrgScopedID")
    return {"OrgScopedID", {{"ID", id(j, "org_scoped_id")}}};
  if (name == "ovr_User_Get") {
    auto m = user_model(id(j, "id"), text(j, "alias"), text(j, "display_name"));
    for (auto [key, field] :
         {std::pair{"profile_url", "ImageUrl"},
          std::pair{"profile_url_small", "SmallImageUrl"},
          std::pair{"token", "InviteToken"}, std::pair{"presence", "Presence"},
          std::pair{"presence_deeplink_message", "PresenceDeeplinkMessage"},
          std::pair{"presence_destination_api_name",
                    "PresenceDestinationApiName"},
          std::pair{"presence_lobby_session_id", "PresenceLobbySessionId"},
          std::pair{"presence_match_session_id", "PresenceMatchSessionId"}})
      m.fields.emplace(field, text(j, key));
    if (j.contains("managed_info") && !j["managed_info"].is_null()) {
      // SDK users/Models/ManagedInfo.cs and signed 1.205 User deserializer.
      Model managed{"ManagedInfo"};
      for (auto [key, field] :
           {std::pair{"department", "Department"}, std::pair{"email", "Email"},
            std::pair{"employee_number", "EmployeeNumber"},
            std::pair{"external_id", "ExternalId"},
            std::pair{"location", "Location"}, std::pair{"manager", "Manager"},
            std::pair{"name", "Name"},
            std::pair{"organization_id", "OrganizationId"},
            std::pair{"organization_name", "OrganizationName"},
            std::pair{"position", "Position"}})
        managed.fields.emplace(field, text(j["managed_info"], key));
      nested(m, "ManagedInfo", std::move(managed));
    }
    m.fields.emplace("PresenceStatus",
                     enumeration(text(j, "presence_status"),
                                 {"UNKNOWN", "ONLINE", "OFFLINE"}));
    return m;
  }
  if (name == "ovr_IAP_ConsumePurchase") {
    // Signed 1.205 dispatcher 0xb0c54 routes consume to the void-message
    // constructor at 0xb05c8, without reading a payload or a success field.
    // Authenticated HTTP success is already checked; honor an explicit false.
    if (!j.is_object() ||
        (j.contains("success") &&
         (!j["success"].is_boolean() || !j["success"].get<bool>())))
      malformed();
    return {};
  }
  return array(j, name == "ovr_IAP_GetProductsBySKU" ||
                      name == "ovr_IAP_GetNextProductArrayPage");
}
struct IapCache::Impl {
  std::filesystem::path file;
  std::mutex mutex;
  std::uint64_t generation = 1;
  std::map<std::string, Json, std::less<>> pending;
  explicit Impl(std::filesystem::path dir)
      : file(dir.empty() ? dir : dir / "iap-durables.json") {}
  Json load(std::string_view account) {
    auto j = Json::parse(venture::paths::read(file, 4 * 1024 * 1024));
    if (j.at("version") != 1 || text(j, "account", true) != account)
      malformed();
    // Cache stores no paging URLs (which may contain tokens), only durable
    // data.
    array(j, false);
    for (const auto& p : elements(j))
      if (product_type(p.at("item")) != 1) malformed();
    return j;
  }
  void save(std::string_view account, Json data) {
    venture::paths::atomic_write(
        file,
        Json{{"version", 1}, {"account", account}, {"data", std::move(data)}}
            .dump());
  }
};
IapCache::IapCache(std::filesystem::path dir)
    : impl_(std::make_unique<Impl>(std::move(dir))) {}
IapCache::~IapCache() = default;
Result IapCache::read(std::string_view account) {
  std::lock_guard lock(impl_->mutex);
  if (impl_->file.empty() || !valid_id(account))
    return {failure(ErrorCode::unavailable)};
  try {
    if (!std::filesystem::exists(impl_->file))
      return {failure(ErrorCode::unavailable)};
    return {{}, {}, {}, array(impl_->load(account), false)};
  } catch (...) {
    log::warn("ovrplatform", "IAP durable cache failed stage=read");
    return {failure(ErrorCode::invalid_response)};
  }
}
std::uint64_t IapCache::generation() {
  std::lock_guard lock(impl_->mutex);
  return impl_->generation;
}
void IapCache::page(std::string_view account, std::string_view url,
                    const Json& page, std::uint64_t generation) {
  std::lock_guard lock(impl_->mutex);
  if (generation != impl_->generation) return;  // a consume crossed this read
  if (impl_->file.empty() || !valid_id(account)) return;
  try {
    Json all = Json::array();
    if (!url.empty()) {
      auto it = impl_->pending.find(url);
      if (it == impl_->pending.end())
        return;  // not a traversal started by this provider
      all = std::move(it->second);
      impl_->pending.erase(it);
    }
    for (const auto& p : elements(page))
      if (product_type(p.at("item")) == 1) {
        // Keep only the SDK purchase schema, never arbitrary echoed server
        // data.
        Json durable{
            {"id", text(p, "id", true)},
            {"grant_time", number(p, "grant_time", true)},
            {"expiration_time", number(p, "expiration_time")},
            {"item",
             {{"sku", text(p.at("item"), "sku", true)}, {"type", "DURABLE"}}},
            {"developer_payload", text(p, "developer_payload")},
            {"reporting_id", text(p, "reporting_id")}};
        all.push_back(std::move(durable));
      }
    if (all.dump().size() > 4 * 1024 * 1024) malformed();
    auto next = next_url(page);
    if (!next.empty()) {
      if (next == url || impl_->pending.size() >= 16) malformed();
      impl_->pending.insert_or_assign(next, std::move(all));
    } else
      impl_->save(account, std::move(all));
  } catch (...) {
    impl_->pending.clear();
    log::warn("ovrplatform", "IAP durable cache failed stage=write");
  }
}
void IapCache::consumed(std::string_view account, std::string_view sku) {
  std::lock_guard lock(impl_->mutex);
  ++impl_->generation;
  impl_->pending.clear();
  if (impl_->file.empty()) return;
  try {
    if (!std::filesystem::exists(impl_->file)) return;
    auto j = impl_->load(account);
    auto& data = j.at("data");
    std::erase_if(data.get_ref<Json::array_t&>(), [&](const auto& p) {
      return text(p.at("item"), "sku") == sku;
    });
    impl_->save(account, std::move(data));
  } catch (...) {
    log::warn("ovrplatform", "IAP durable cache failed stage=consume");
  }
}
}  // namespace venture::ovrplatform
