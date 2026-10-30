#pragma once
#include "common/meta/auth.h"
#include <set>
namespace venture::meta {
struct Game {
  std::string id, name, description, publisher, image, platform,
      ownership_account;
  std::string package;  // Android package of the live build; empty when unknown
  std::string icon;     // the listing's square app icon; not cached in library.json
  bool owned = false;
  bool system = false; // a Quest OS component or home environment
  uint64_t recent = 0;
};
struct Snapshot {
  std::vector<Game> games;
  std::string account_id, alias, display_name, warning;
  bool complete = false;
};
struct File {
  std::string id, name, uri, kind;
  uint64_t size = 0;
  bool required = true;
  std::string sku, iap_id, offer_id;
  std::string asset_type, metadata, language_tag, language_english_name,
      language_native_name;
  std::string sha256;  // lowercase hex, recorded for imported files
  bool base() const { return required && sku.empty() && iap_id.empty(); }
};
struct Plan {
  std::string app_id, binary_id, package, version, version_code;
  std::vector<File> files;
  std::vector<std::string> installed_addons;
  // False when Meta's full binary query failed: files hold the APK and OBB and
  // at most each asset's id, name and size (no IAP, language or required
  // flags), or no assets at all.
  bool assets_complete = true;
  // False for a manifest another installer wrote (the legacy CLI or an
  // import): it lacks the launcher's installedAddons record and may lack catalog
  // metadata. The launcher upgrades such plans from Meta before a launch.
  bool launcher_format = true;
  // Installed from an import that matched no Meta build ("source": "import",
  // empty binaryId): the source's own files. appId is Meta's id for the app
  // when known; otherwise plan.json leaves it empty and app_id holds the
  // package as the launcher's key.
  bool local = false;
  uint64_t size() const; // Required, ungated base-game bytes; files includes
                         // the full catalog.
  Json json() const;     // Intentionally excludes URIs and credentials.
};
// Android's [main|patch].<version>.<package>.obb expansion file name.
bool expansion_name(std::string_view name, std::string_view package);
struct Addon {
  std::string sku, name, description, price_text, image_url;
  uint64_t size = 0;
  bool owned = false, installed = false, downloading = false;
  uint64_t done = 0, total = 0;
  std::string error;
};
struct Details {
  Game game;
  std::string live_id, version, version_code;
  uint64_t size = 0;
  Plan plan;
};
inline constexpr const char *catalog_unavailable =
    "Meta's asset catalog is temporarily unavailable. Try again later.";
// Throws catalog_unavailable for a plan without its asset catalog.
void require_asset_catalog(const Plan &plan);
std::vector<Addon> group_addons(const Plan &plan);
// Whether `catalog` may replace the installed `previous` plan: never a reduced
// catalog over a full one, nor one that lists fewer asset files.
bool catalog_replaces(const Plan &catalog, const Plan &previous);
bool valid_sku(std::string_view sku);
std::set<std::string> parse_owned_skus(const Json& rows);
Json nodes(const Json &j);
bool quest_app(const Json &j);
Game card(const Json &item);
Snapshot parse_library(const Json &response);
// The 16:9 landscape cover named by a public store page: its schema.org
// primaryImageOfPage, else its og:image. Empty when the page names neither.
std::string cover_from_page(std::string_view html);
Plan build_plan(std::string_view app_id, const Json &binary);
Plan parse_plan(const Json &json);
class Store {
public:
  using Transport = std::function<Json(std::string_view, const http::Fields &)>;
  using ReadTransport =
      std::function<http::Response(std::string_view, const http::Fields &)>;
  using ListingTransport = std::function<Json(std::string_view)>;
  // The file name a delivery URL serves (http::delivery_file_name).
  using NameTransport = std::function<std::string(std::string_view)>;
  explicit Store(std::string token, std::stop_token stop = {},
                 Transport transport = {}, ReadTransport read = {},
                 ListingTransport listing = {}, NameTransport names = {})
      : token_(std::move(token)), stop_(stop), transport_(std::move(transport)),
        read_(std::move(read)), listing_(std::move(listing)), names_(std::move(names)) {}
  Json query(std::string_view doc, const Json &vars = Json::object()) const;
  // The signed-in account alone (id, alias, display name), without the
  // library: sign-in does not depend on listing every entitlement.
  Snapshot identity() const;
  Snapshot library() const;
  Details details(std::string_view id) const;
  Game artwork(std::string_view id) const;
  // The landscape cover from the public store page. Graph only offers the
  // wide hero banner and the square icon.
  std::string cover(std::string_view id) const;
  Json builds(std::string_view id) const;
  // One app's listing as this account sees it: the app alone, owned, when
  // Meta says the account owns it (the whole-library query fails for some
  // accounts); no games when it does not.
  Snapshot owned_listing(std::string_view id) const;
  // Whether this account owns the app: its listing's answer, else the library.
  bool owns(std::string_view id) const;
  Plan owned_plan(std::string_view id) const;
  // Installed builds use their own binary's catalog, never a newer build's
  // files.
  Plan owned_plan(std::string_view id, std::string_view binary_id) const;
  std::vector<Addon> addons(const Plan &plan) const;
  std::set<std::string> owned_skus(std::string_view app_id) const;
  std::string download_url(const File &file) const;

private:
  Snapshot headset_library() const;
  Json binary_details(std::string_view id) const;
  Json fallback_assets(std::string_view id) const;
  std::string token_;
  mutable std::string delivery_;
  std::stop_token stop_;
  Transport transport_;
  ReadTransport read_;
  ListingTransport listing_;
  NameTransport names_;
  Json public_item(std::string_view id) const;
  std::string app_token(std::string_view app_id) const;
  Json commerce(std::string_view edge, const http::Fields &fields,
                const std::string &app_token) const;
  Json request(std::string_view url, const http::Fields &fields) const;
};
} // namespace venture::meta
