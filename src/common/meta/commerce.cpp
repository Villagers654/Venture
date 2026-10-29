#include "common/meta/store.h"
#include <algorithm>
#include <chrono>

namespace venture::meta {
namespace {
// Meta's signed Platform DLL 1.205: GetProductsBySKU builder 0x154FE0,
// fields at RVA 0x8997F0; GetViewerPurchases builder 0x155380. Source and
// request shape: dev-cache/design/online/plan.md sections 2.0 and 2.4.
constexpr auto product_fields =
    "sku,current_offer{description,name,price{currency,amount_in_hundredths,"
    "formatted}},"
    "type,short_description,icon_url,cover_url,"
    "content_rating{age_rating_text,descriptors,interactive_elements,rating_"
    "definition_uri,age_rating_image_uri},"
    "billing_plans{trial_offers{price{currency,amount_in_hundredths,formatted},"
    "trial_term,trial_type,max_term_count},"
    "paid_offer{price{currency,amount_in_hundredths,formatted},subscription_"
    "term}}";
constexpr auto purchase_fields = "id,grant_time,expiration_time,item{sku,type},"
                                 "developer_payload,reporting_id";
uint64_t purchase_time(const Json &value, bool optional = false) {
  if (optional && value.is_null())
    return 0;
  auto text = str(value);
  uint64_t result = 0;
  auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), result);
  if (error != std::errc{} || end != text.data() + text.size())
    throw std::runtime_error("Meta returned invalid purchase times.");
  return result;
}
}  // namespace
std::set<std::string> parse_owned_skus(const Json& rows) {
  if (!rows.is_array()) throw std::runtime_error("Invalid purchase list.");
  std::set<std::string> owned;
  auto now =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  for (const auto &row : rows) {
    const auto &item = field(row, "item");
    auto sku = str(item, "sku"), type = str(item, "type");
    if (!valid_sku(sku) || type.empty())
      throw std::runtime_error("Meta returned incomplete add-on ownership.");
    std::transform(type.begin(), type.end(), type.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    auto expiry = purchase_time(field(row, "expiration_time"), true);
    auto grant = purchase_time(field(row, "grant_time"));
    if (type == "durable" && grant <= uint64_t(now) &&
        (!expiry || expiry > uint64_t(now)))
      owned.insert(std::move(sku));
  }
  return owned;
}

std::string Store::app_token(std::string_view app_id) const {
  if (!digits(app_id, 5) || token_.empty())
    throw std::runtime_error("Sign in to retrieve add-ons.");
  auto j = request("https://graph.oculus.com/authenticate_application",
                   {{"app_id", std::string(app_id)}, {"access_token", token_}});
  auto token = str(j, "access_token");
  if (token.empty() || token.find_first_of("\r\n") != token.npos)
    throw std::runtime_error("Meta did not return an application token.");
  return token;
}
Json Store::commerce(std::string_view edge, const http::Fields &fields,
                     const std::string &token) const {
  if (edge != "available_purchases" && edge != "viewer_purchases")
    throw std::runtime_error("Invalid Meta commerce endpoint.");
  auto params = fields;
  params.emplace_back("app_version", "0");
  const auto path = "/v1.20/application/" + std::string(edge);
  auto url = "https://graph.oculus.com" + path + '?' + http::form(params);
  Json rows = Json::array();
  std::set<std::string> seen;
  for (unsigned page = 0; page < 16; ++page) {
    // Paging never sends either credential to a different host or edge.
    auto u = http::parse_url(url);
    if (u.scheme != "https" || u.host != "graph.oculus.com" ||
        (!u.port.empty() && u.port != "443") || !u.user.empty() ||
        !u.password.empty() || u.path != path || url.find('#') != url.npos ||
        !seen.insert(url).second)
      throw std::runtime_error("Meta returned invalid add-on pagination.");
    for (auto &[key, value] : http::query(u.query))
      if (key == "access_token")
        throw std::runtime_error(
            "Meta returned credential-bearing pagination.");
    if (stop_.stop_requested())
      throw std::runtime_error("Transfer paused.");
    http::Fields headers{{"Authorization", "Bearer " + token}};
    auto response =
        read_ ? read_(url, headers) : http::get_meta(url, headers, stop_);
    auto j = Json::parse(response.body, nullptr, false);
    if (response.status != 200 || j.is_discarded() ||
        !field(j, "error").is_null() || !field(j, "errors").is_null()) {
      // Server text and response bodies may contain account data or tokens.
      auto code = number(field(j, "error"), "code");
      throw std::runtime_error(
          "Meta add-on read failed (HTTP " + std::to_string(response.status) +
          ", code " + std::to_string(code) + "). Refresh or sign in again.");
    }
    auto &data = field(j, "data");
    if (!data.is_array())
      throw std::runtime_error("Meta returned an invalid add-on list.");
    for (auto &row : data) {
      if (!row.is_object())
        throw std::runtime_error("Meta returned an incomplete add-on list.");
      rows.push_back(row);
    }
    auto &next = field(field(j, "paging"), "next");
    if (next.is_null() || next == "")
      return rows;
    if (!next.is_string())
      throw std::runtime_error("Meta returned invalid add-on pagination.");
    url = next.get<std::string>();
  }
  throw std::runtime_error("Meta returned too many add-on pages.");
}
std::set<std::string> Store::owned_skus(std::string_view app_id) const {
  return parse_owned_skus(commerce(
      "viewer_purchases", {{"fields", purchase_fields}}, app_token(app_id)));
}
std::vector<Addon> Store::addons(const Plan &plan) const {
  auto result = group_addons(plan);
  if (result.empty())
    return result;
  std::string skus;
  for (auto &addon : result) {
    if (!skus.empty())
      skus += ',';
    skus += addon.sku;
  }
  auto token = app_token(plan.app_id);
  auto products = commerce("available_purchases",
                           {{"fields", product_fields}, {"skus", skus}}, token);
  auto owned = parse_owned_skus(
      commerce("viewer_purchases", {{"fields", purchase_fields}}, token));
  std::set<std::string> seen;
  for (const auto &product : products) {
    auto sku = str(product, "sku");
    auto found = std::ranges::find(result, sku, &Addon::sku);
    if (!valid_sku(sku) || !seen.insert(sku).second || found == result.end())
      throw std::runtime_error("Meta returned unexpected add-on products.");
    auto &offer = field(product, "current_offer");
    found->name = str(offer, "name");
    found->description = str(offer, "description");
    if (found->description.empty())
      found->description = str(product, "short_description");
    found->price_text = str(field(offer, "price"), "formatted");
    found->image_url = str(product, "cover_url");
    if (found->image_url.empty())
      found->image_url = str(product, "icon_url");
  }
  for (auto &addon : result) {
    addon.owned = owned.contains(addon.sku);
    if (addon.name.empty()) {
      // Fallback source: public Quest storefront doc 6549406941839522, already
      // used by Store::artwork. The binary's iap_item.id is a storefront item;
      // no profile/app credential is sent to www.meta.com. Retired Epic bundles
      // still have names/artwork there, but current_offer is null (no price).
      auto file = std::ranges::find(plan.files, addon.sku, &File::sku);
      try {
        auto item = public_item(file->iap_id);
        if (str(item, "display_name").empty())
          throw std::runtime_error("Public add-on name is unavailable.");
        auto display = card(item);
        addon.name = display.name;
        addon.description = display.description;
        addon.image_url = display.image;
        addon.price_text =
            str(field(field(item, "current_offer"), "price"), "formatted");
        if (addon.price_text.empty()) {
          addon.price_text = "Unavailable";
          addon.error = "Meta has no current offer for this add-on.";
        }
      } catch (...) {
        if (stop_.stop_requested())
          throw std::runtime_error("Transfer paused.");
        addon.error =
            "Public Meta add-on details are unavailable. Refresh to retry.";
      }
    }
    if (addon.name.empty())
      addon.name = addon.sku;
  }
  return result;
}

Plan Store::owned_plan(std::string_view id, std::string_view binary_id) const {
  if (!digits(id, 5) || !digits(binary_id))
    throw std::runtime_error("Invalid application build.");
  if (!owns(id))
    throw std::runtime_error("Meta did not confirm ownership of this game.");
  auto binary = binary_details(binary_id);
  auto parent = str(field(binary, "binary_application"), "id");
  if (parent.empty()) {
    auto list = builds(id);
    if (std::ranges::none_of(
            list, [&](auto &b) { return str(b, "id") == binary_id; }))
      throw std::runtime_error(
          "Meta could not verify this build belongs to the game.");
  } else if (parent != id)
    throw std::runtime_error("Meta returned a different game's build.");
  return build_plan(id, binary);
}
} // namespace venture::meta
