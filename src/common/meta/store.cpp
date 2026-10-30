#include "common/meta/store.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <thread>

#include "common/log.h"
namespace venture::meta {
namespace {
// Waits `delay` unless a stop is requested first; false when stopped.
bool pause_for(std::stop_token stop, std::chrono::milliseconds delay) {
  std::mutex mutex;
  std::condition_variable_any wake;
  std::unique_lock lock(mutex);
  wake.wait_for(lock, stop, delay, [] { return false; });
  return !stop.stop_requested();
}
void require_id(std::string_view id) {
  if (!digits(id, 5))
    throw std::runtime_error("Invalid Quest application ID.");
}
std::string upper_ascii(std::string text) {
  for (char &letter : text)
    letter = char(std::toupper(static_cast<unsigned char>(letter)));
  return text;
}
std::string platform(const Json &record) {
  for (const auto *source : {&record, &field(record, "latest_supported_binary")}) {
    auto declared = str(*source, "platform");
    if (!declared.empty())
      return upper_ascii(std::move(declared));
  }
  return {};
}
const Json &connection(const Json &response) {
  const auto &viewer = field(field(response, "data"), "viewer");
  const auto &owner = field(viewer, "user");
  if (!field(owner, "active_entitlements").is_null())
    return owner["active_entitlements"];
  return field(viewer, "active_entitlements");
}
bool android_build(const Json &record) {
  if (str(record, "__typename") == "RiftBinary")
    return false;
  return platform(record) != "PC";
}
uint64_t build_revision(const Json &record) {
  for (auto key : {"version_code", "versionCode"})
    if (auto revision = number(record, key))
      return revision;
  return 0;
}
bool token_endpoint(const http::Url &destination) {
  if (destination.scheme != "https" || !destination.user.empty() ||
      !destination.password.empty() ||
      (!destination.port.empty() && destination.port != "443") ||
      destination.path != "/binaries/download/")
    return false;
  std::string_view service(destination.host);
  constexpr std::string_view suffix = ".oculus.com";
  if (!service.ends_with(suffix))
    return false;
  service.remove_suffix(suffix.size());
  if (service == "securecdn")
    return true;
  if (!service.starts_with("securecdn-"))
    return false;
  service.remove_prefix(std::string_view("securecdn-").size());
  return !service.empty() && service.find_first_not_of(
      "abcdefghijklmnopqrstuvwxyz0123456789-") == service.npos;
}
} // namespace
bool expansion_name(std::string_view name, std::string_view package) {
  // Android's APK Expansion Files naming contract is
  // [main|patch].<expansion-version>.<package-name>.obb. The expansion version
  // may precede the current APK version: developer.android.com/google/play/expansion-files.
  if (name.starts_with("main.")) name.remove_prefix(5);
  else if (name.starts_with("patch.")) name.remove_prefix(6);
  else return false;
  const auto suffix = std::string(".") + std::string(package) + ".obb";
  if (!name.ends_with(suffix)) return false;
  name.remove_suffix(suffix.size());
  return digits(name);
}
Json nodes(const Json &j) {
  if (j.is_array())
    return j;
  auto &n = field(j, "nodes");
  if (n.is_array())
    return n;
  auto &edges = field(j, "edges");
  Json result = Json::array();
  if (edges.is_array())
    for (auto &edge : edges) {
      if (!field(edge, "node").is_object())
        throw std::runtime_error(
            "Meta returned an incomplete list. Previous data was kept.");
      result.push_back(edge["node"]);
    }
  else
    throw std::runtime_error(
        "Meta returned an invalid list. Previous data was kept.");
  return result;
}
bool quest_app(const Json &record) {
  const auto declared = platform(record);
  if (declared == "ANDROID_6DOF")
    return true;
  if (!declared.empty() && declared != "ANDROID")
    return false;
  if (declared.empty() &&
      str(field(record, "latest_supported_binary"), "__typename") != "AndroidBinary")
    return false;
  static const std::set<std::string> headsets{
      "EUREKA", "HOLLYWOOD", "MONTEREY", "PANTHER", "QUEST", "QUEST_2",
      "QUEST_3", "QUEST_3S", "QUEST_PRO", "SEACLIFF"};
  for (auto key : {"supported_hmd_types", "targeted_devices", "supported_hmd_platforms"}) {
    const auto &devices = field(record, key);
    if (!devices.is_array())
      continue;
    for (const auto &device : devices)
      if (headsets.contains(upper_ascii(str(device))))
        return true;
  }
  return false;
}
Game card(const Json &j) {
  Game g;
  g.id = str(j, "id");
  g.name = str(j, "display_name");
  if (g.name.empty())
    g.name = str(j, "displayName");
  if (g.name.empty())
    g.name = str(j, "name");
  if (g.name.empty())
    g.name = "Quest app " + g.id;
  g.description = str(j, "display_long_description");
  if (g.description.empty())
    g.description = str(j, "display_short_description");
  g.publisher = str(j, "publisher_name");
  if (g.publisher.empty())
    g.publisher = str(j, "developer_name");
  for (auto key : {"cover_landscape_image", "hero", "medium_landscape_image",
                   "cover_square_image", "icon_image"}) {
    g.image = str(field(j, key), "uri");
    if (!g.image.empty())
      break;
  }
  for (auto key : {"icon_image", "cover_square_image"}) {
    g.icon = str(field(j, key), "uri");
    if (!g.icon.empty())
      break;
  }
  g.platform = platform(j);
  // Quest OS components and home environments are entitlements too, but not
  // games: first-party packages the store never offers, and environments.
  auto package = str(field(j, "latest_supported_binary"), "package_name");
  if (package.empty())
    package = str(j, "package_name");
  if (paths::safe_package(package))
    g.package = package;
  bool first_party = package.starts_with("com.oculus.") || package.starts_with("com.meta.") ||
                     package.starts_with("com.facebook.");
  g.system = package.find(".environment.") != std::string::npos || package.starts_with("com.meta.shell.env.") ||
             (first_party && ((j.contains("baseline_offer") && j["baseline_offer"].is_null()) ||
                              field(j, "uninstall_restricted") == true));
  return g;
}
Snapshot parse_library(const Json &response) {
  Snapshot s;
  auto &c = connection(response);
  auto list = nodes(c);
  bool classified = true;
  std::set<std::string> seen;
  for (auto &e : list) {
    auto &item = field(e, "item");
    auto id = str(item, "id");
    if (!item.is_object() || !digits(id, 5))
      throw std::runtime_error(
          "Meta returned incomplete entitlements. Previous library was kept.");
    if (quest_app(item)) {
      if (seen.insert(id).second) {
        auto g = card(item);
        g.owned = true;
        s.games.push_back(g);
      }
    } else if (platform(item) != "PC" &&
               str(field(item, "latest_supported_binary"), "__typename") !=
                   "RiftBinary")
      classified = false;
  }
  auto &page = field(c, "page_info");
  if (c.is_object() && c.contains("page_info") &&
      !field(page, "has_next_page").is_boolean())
    throw std::runtime_error(
        "Meta returned invalid pagination. Previous library was kept.");
  s.complete = field(page, "has_next_page") == false && classified;
  if (!s.complete)
    s.warning = "Meta returned a partial or unconfirmed library. Previously "
                "confirmed titles were kept.";
  auto &user = field(field(field(response, "data"), "viewer"), "user");
  s.account_id = str(user, "id");
  if (!s.account_id.empty() && !digits(s.account_id))
    throw std::runtime_error("Meta returned an invalid account identity.");
  s.alias = str(user, "alias");
  s.display_name = str(user, "display_name");
  if (s.alias.empty())
    s.alias = s.display_name;
  return s;
}
uint64_t Plan::size() const {
  uint64_t s = 0;
  for (auto &f : files)
    if (f.base()) {
      if (f.size > UINT64_MAX - s)
        throw std::runtime_error("Invalid base content size.");
      s += f.size;
    }
  return s;
}
Json Plan::json() const {
  Json files_json = Json::array();
  for (auto &f : files)
    files_json.push_back(
        {{"id", f.id},
         {"name", f.name},
         {"size", f.size},
         {"kind", f.kind},
         {"required", f.required},
         {"asset_type", f.asset_type},
         {"metadata", f.metadata},
         {"language", f.language_tag.empty()
                          ? Json{}
                          : Json{{"tag", f.language_tag},
                                 {"english_name", f.language_english_name},
                                 {"native_name", f.language_native_name}}},
         {"iap_item",
          f.iap_id.empty()
              ? Json{}
              : Json{{"id", f.iap_id},
                     {"sku", f.sku},
                     {"public_offer", f.offer_id.empty()
                                          ? Json{}
                                          : Json{{"id", f.offer_id}}}}}});
  for (std::size_t i = 0; i < files.size(); ++i)
    if (!files[i].sha256.empty())
      files_json[i]["sha256"] = files[i].sha256;
  Json plan = {{"appId", local && !digits(app_id, 5) ? std::string() : app_id},
               {"binaryId", binary_id},
               {"package", package},
               {"version", version},
               {"versionCode", number(Json(version_code))},
               {"files", files_json},
               {"installedAddons", installed_addons}};
  if (!assets_complete)
    plan["assetCatalog"] = "unavailable";
  if (local)
    plan["source"] = "import";
  return plan;
}
void require_asset_catalog(const Plan &plan) {
  if (!plan.assets_complete)
    throw std::runtime_error(catalog_unavailable);
}
bool valid_sku(std::string_view sku) {
  // Comma separates SKUs in Meta's GetProductsBySKU request (DLL 0x154FE0).
  return !sku.empty() && sku.size() <= 256 &&
         std::ranges::none_of(sku, [](unsigned char c) {
           return c < 32 || c == 127 || c == ',';
         });
}
bool catalog_replaces(const Plan &catalog, const Plan &previous) {
  if (catalog.assets_complete)
    return true;
  auto assets = [](const Plan &p) {
    return std::ranges::count(p.files, std::string("asset"), &File::kind);
  };
  return !previous.assets_complete && assets(catalog) >= assets(previous);
}
std::vector<Addon> group_addons(const Plan &plan) {
  std::vector<Addon> result;
  for (const auto &f : plan.files) {
    if (f.sku.empty())
      continue;
    if (!valid_sku(f.sku))
      throw std::runtime_error("Invalid add-on SKU.");
    auto found = std::ranges::find(result, f.sku, &Addon::sku);
    if (found == result.end()) {
      Addon addon;
      addon.sku = f.sku;
      result.push_back(std::move(addon));
      found = result.end() - 1;
    }
    if (f.size > UINT64_MAX - found->size)
      throw std::runtime_error("Invalid add-on content size.");
    found->size += f.size;
  }
  return result;
}
namespace {
// The store query names an asset's kind asset_file_type (DEFAULT, STORE,
// LANGUAGE_PACK); the Platform SDK spells it default, store, language_pack.
std::string asset_type(const Json &file) {
  auto type = str(file, "asset_type");
  if (!type.empty()) return type;
  type = str(file, "asset_file_type");
  std::transform(type.begin(), type.end(), type.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return type == "default" || type == "store" || type == "language_pack" ? type : "";
}
// Meta lists no language for a language pack; its file is named for it
// (Asgard's Wrath 2: ko.lang, zh.lang). Only a BCP-47 shaped stem counts.
std::string language_pack_tag(std::string_view name) {
  auto tag = std::string(name.substr(0, name.find('.')));
  std::replace(tag.begin(), tag.end(), '_', '-');
  std::size_t start = 0;
  for (std::size_t part = 0; start <= tag.size(); ++part) {
    auto end = std::min(tag.find('-', start), tag.size());
    auto length = end - start;
    if (!length || length > 8 || (part == 0 && (length < 2 || length > 3)) ||
        !std::all_of(tag.begin() + start, tag.begin() + end,
                     [&](unsigned char c) { return part ? std::isalnum(c) : std::isalpha(c); }))
      return {};
    start = end + 1;
  }
  return tag;
}
}  // namespace
Plan build_plan(std::string_view app_id, const Json &b) {
  require_id(app_id);
  Plan p{std::string(app_id), str(b, "id"),           str(b, "package_name"),
         str(b, "version"),   str(b, "version_code"), {}};
  if (p.version_code.empty())
    p.version_code = str(b, "versionCode");
  if (p.version_code.empty())
    p.version_code = p.binary_id;
  auto parent = str(field(b, "binary_application"), "id");
  if (!paths::safe_package(p.package) || !digits(p.binary_id) ||
      !digits(p.version_code) || platform(b) == "PC" ||
      str(b, "__typename") == "RiftBinary" ||
      (!parent.empty() && parent != app_id))
    throw std::runtime_error(
        "Meta did not return an Android build for this game.");
  std::set<std::string> names;
  auto add = [&](const Json &file, std::string name, std::string kind) {
    if (!paths::safe_name(name))
      throw std::runtime_error("Meta returned an unsafe content filename.");
    std::string key = name;
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    if (key == "plan.json" || key == "plan.json.tmp" || key.ends_with(".part"))
      throw std::runtime_error(
          "Meta returned a filename reserved for installation state.");
    File f{str(file, "id"), name, str(file, "uri"), kind, number(file, "size")};
    if (kind == "asset") {
      f.asset_type = asset_type(file);
      f.metadata = str(file, "metadata");
      f.language_tag = str(field(file, "language"), "tag");
      if (f.language_tag.empty() && f.asset_type == "language_pack")
        f.language_tag = language_pack_tag(name);
      f.language_english_name = str(field(file, "language"), "english_name");
      f.language_native_name = str(field(file, "language"), "native_name");
      f.required = field(file, "is_required") == true;
      auto &iap = field(file, "iap_item");
      if (!iap.is_null() && iap != false) {
        f.iap_id = str(iap, "id");
        f.sku = str(iap, "sku");
        f.offer_id = str(field(iap, "public_offer"), "id");
        if (!digits(f.iap_id) || (!f.sku.empty() && !valid_sku(f.sku)) ||
            (!f.offer_id.empty() && !digits(f.offer_id)))
          throw std::runtime_error("Meta returned invalid add-on metadata.");
      }
    }
    if ((!digits(f.id) && f.uri.empty()) ||
        (!f.uri.empty() && !http::allowed_delivery(f.uri)) || !f.size)
      throw std::runtime_error(
          "Meta omitted valid file details or size. Download was not started.");
    if (!names.insert(key).second) {
      for (auto &existing : p.files)
        if (existing.name == f.name && existing.id == f.id &&
            existing.size == f.size && existing.kind == f.kind &&
            existing.required == f.required && existing.sku == f.sku &&
            existing.iap_id == f.iap_id && existing.offer_id == f.offer_id)
          return;
      throw std::runtime_error(
          "Meta returned conflicting file names. Download was not started.");
    }
    p.files.push_back(std::move(f));
  };
  add(b, p.package + ".apk", "apk");
  p.assets_complete = field(b, "asset_files_unavailable") != true;
  auto &obb = field(b, "obb_binary");
  if (!obb.is_null() && !str(obb, "id").empty())
    add(obb, str(obb, "file_name"), "obb");
  auto &assets = field(b, "asset_files");
  if (!assets.is_null()) {
    auto list = nodes(assets);
    if (number(assets, "count") > list.size() ||
        field(field(assets, "page_info"), "has_next_page") == true)
      throw std::runtime_error(
          "Meta's asset list was incomplete, so the download did not start.");
    for (auto &a : list) {
      auto name = str(a, "file_name");
      add(a, name, "asset");
    }
  }
  return p;
}
Plan parse_plan(const Json &j) {
  Plan p{str(j, "appId"),   str(j, "binaryId"),    str(j, "package"),
         str(j, "version"), str(j, "versionCode"), {}};
  p.local = str(j, "source") == "import" && p.binary_id.empty();
  if (p.local && p.app_id.empty())
    p.app_id = p.package;
  if (p.version_code.empty())
    p.version_code = p.binary_id;
  if ((p.local ? !digits(p.app_id, 5) && p.app_id != p.package
               : !digits(p.app_id, 5) || !digits(p.binary_id)) ||
      !paths::safe_package(p.package) || !digits(p.version_code) ||
      !field(j, "files").is_array() ||
      (j.contains("assetCatalog") && j["assetCatalog"] != "unavailable"))
    throw std::runtime_error("Invalid installation plan.");
  p.assets_complete = !j.contains("assetCatalog");
  p.launcher_format = j.contains("installedAddons");
  std::set<std::string> names;
  bool apk = false;
  // The original meta_cli.py saved only required, ungated base assets, but
  // classified every .obb asset as an expansion file. Recognize that exact
  // four-field representation and retain its primary Android expansion;
  // recover additional AssetFile entries in memory without rewriting the plan
  // or guessing language/IAP metadata. Current catalogs keep their own kinds.
  const bool legacy_cli = std::ranges::all_of(j["files"], [](const Json& f) {
    return f.is_object() && f.size() == 4 && f.contains("id") &&
           f.contains("name") && f.contains("kind") && f.contains("size");
  }) && std::ranges::any_of(j["files"], [&](const Json& f) {
    return str(f, "kind") == "obb" && expansion_name(str(f, "name"), p.package);
  });
  for (auto &f : j["files"]) {
    auto name = str(f, "name");
    auto key = name;
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    if (!paths::safe_name(name) || !names.insert(key).second ||
        key == "plan.json" || key == "plan.json.tmp" || key.ends_with(".part"))
      throw std::runtime_error("Invalid installation filename.");
    File file{str(f, "id"), name, {}, str(f, "kind"), number(f, "size")};
    if (legacy_cli && file.kind == "obb" && name.ends_with(".obb") &&
        !expansion_name(name, p.package))
      file.kind = "asset";
    file.asset_type = str(f, "asset_type");
    file.metadata = str(f, "metadata");
    file.language_tag = str(field(f, "language"), "tag");
    file.language_english_name = str(field(f, "language"), "english_name");
    file.language_native_name = str(field(f, "language"), "native_name");
    if (f.contains("required") && !f["required"].is_boolean())
      throw std::runtime_error("Invalid installation asset metadata.");
    file.required = field(f, "required") != false;
    file.sha256 = str(f, "sha256");
    if (!file.sha256.empty() &&
        (file.sha256.size() != 64 ||
         file.sha256.find_first_not_of("0123456789abcdef") != std::string::npos))
      throw std::runtime_error("Invalid installation file hash.");
    auto &iap = field(f, "iap_item");
    file.sku = str(iap, "sku");
    file.iap_id = str(iap, "id");
    file.offer_id = str(field(iap, "public_offer"), "id");
    if ((!file.sku.empty() && !valid_sku(file.sku)) ||
        (!file.iap_id.empty() && !digits(file.iap_id)) ||
        (!iap.is_null() && (!iap.is_object() || !digits(file.iap_id))) ||
        (!file.offer_id.empty() && !digits(file.offer_id)) ||
        (file.kind != "asset" &&
         (!file.sku.empty() || !file.iap_id.empty() || !file.required)))
      throw std::runtime_error("Invalid installation asset metadata.");
    p.files.push_back(std::move(file));
    apk |= str(f, "kind") == "apk" && name.ends_with(".apk");
  }
  if (!apk)
    throw std::runtime_error("Installation has no APK.");
  if (j.contains("installedAddons")) {
    if (!j["installedAddons"].is_array())
      throw std::runtime_error("Invalid installed add-ons.");
    auto catalog = group_addons(p);
    std::set<std::string> seen;
    for (auto &entry : j["installedAddons"]) {
      auto sku = str(entry);
      if (!valid_sku(sku) || !seen.insert(sku).second ||
          std::ranges::find(catalog, sku, &Addon::sku) == catalog.end())
        throw std::runtime_error("Invalid installed add-ons.");
      p.installed_addons.push_back(std::move(sku));
    }
  }
  return p;
}
Json Store::query(std::string_view doc, const Json &vars) const {
  if (token_.empty())
    throw std::runtime_error("Not signed in to Meta.");
  return request("https://graph.oculus.com/graphql",
                 {{"access_token", token_},
                  {"doc_id", std::string(doc)},
                  {"variables", vars.dump()}});
}
Json Store::request(std::string_view url, const http::Fields &fields) const {
  return transport_ ? transport_(url, fields) : post(url, fields, stop_);
}
Snapshot Store::identity() const {
  if (token_.empty())
    throw std::runtime_error("Not signed in to Meta.");
  auto j = request("https://graph.oculus.com/me", {{"access_token", token_},
                                                   {"fields", "id,alias,display_name"},
                                                   {"method", "GET"}});
  Snapshot s;
  s.account_id = str(j, "id");
  if (!digits(s.account_id))
    throw std::runtime_error("Meta returned an invalid account identity.");
  s.alias = str(j, "alias");
  s.display_name = str(j, "display_name");
  if (s.alias.empty())
    s.alias = s.display_name;
  return s;
}
Snapshot Store::library() const {
  // The whole library is one reply (the query has no paging); a one-off 5xx
  // is retried before it counts. Meta fails it for some large libraries, so
  // a persistent 5xx lists the headset's library instead, a page at a time.
  for (int attempt = 0;; ++attempt) {
    try {
      return parse_library(query("4850747515044496"));
    } catch (const RequestError &error) {
      if (error.status < 500 || error.status >= 600 || stop_.stop_requested()) throw;
      log::warn("meta", "Library query (doc 4850747515044496) attempt {} failed: {}", attempt + 1, error.what());
      if (attempt == 2 || !pause_for(stop_, std::chrono::milliseconds(500 << attempt))) {
        if (!stop_.stop_requested()) {
          try {
            return headset_library();
          } catch (const std::exception &fallback) {
            log::warn("meta", "Headset library query failed too: {}", fallback.what());
          }
        }
        std::string reason = error.what();
        if (auto end = reason.find(". Try signing in again."); end != reason.npos)
          reason.resize(end);
        throw RequestError(error.status, "Meta could not list your games right now (" + reason +
                                             "). Use Refresh in the library to try again.");
      }
    }
  }
}
// The library query a Quest itself runs: Android entitlements only, paged
// by cursor. The variables are the headset's own; a persisted query takes
// the variables it was built with.
Snapshot Store::headset_library() const {
  Json vars = {{"smallLandscapeImageSize", "180x100"},
               {"skipFetchingRequiredUpdates", true},
               {"coverSquareImageSize", "225x225"},
               {"iconImageSize", "64x64"},
               {"landscapeImageSize", "405x720"},
               {"fetch_fallback_uris", true},
               {"machine_id", "1"},
               {"imageScale", 1.0},
               {"cursorID", nullptr},
               {"count", 100},
               {"allFields", true},
               {"onlyDefaultApps", false},
               {"excludePackages", nullptr},
               {"fetchAchievementBasedTrialInfo", false},
               {"compatibilityFilter", "DEFAULT"}};
  auto s = identity();
  std::set<std::string> seen;
  // A cursor that never advances, or a library past any real size, stops
  // the paging; what was listed is kept as partial.
  for (int page = 0; page < 100 && !stop_.stop_requested(); ++page) {
    auto response = request("https://graph.oculus.com/graphql",
                            {{"access_token", token_},
                             {"client_doc_id", "412097616712440770934752951129"},
                             {"variables", vars.dump()}});
    auto &c = field(field(field(response, "data"), "entitlements"), "active_android_entitlements");
    if (!field(c, "edges").is_array())
      throw std::runtime_error("Meta returned an invalid list. Previous library was kept.");
    for (auto &e : nodes(c)) {
      auto &item = field(e, "item");
      auto id = str(item, "id");
      if (!item.is_object() || !digits(id, 5))
        throw std::runtime_error("Meta returned incomplete entitlements. Previous library was kept.");
      if (quest_app(item) && seen.insert(id).second) {
        auto g = card(item);
        g.owned = true;
        s.games.push_back(std::move(g));
      }
    }
    auto &info = field(c, "page_info");
    if (field(info, "has_next_page") == false) {
      s.complete = true;
      break;
    }
    auto cursor = str(info, "end_cursor");
    if (cursor.empty() || vars["cursorID"] == cursor)
      break;
    vars["cursorID"] = cursor;
  }
  if (!s.complete)
    s.warning = "Meta returned a partial or unconfirmed library. Previously "
                "confirmed titles were kept.";
  log::info("meta", "Listed {} Quest titles from the headset library query; complete={}", s.games.size(), s.complete);
  return s;
}
Json Store::binary_details(std::string_view id) const {
  require_id(id);
  // A one-off 5xx is retried, so only a persistent failure takes the reduced
  // fallback below.
  for (int attempt = 0;; ++attempt) {
    try {
      auto response = query("4734929166632773", {{"binaryID", id}});
      auto binary = field(field(response, "data"), "node");
      if (!binary.is_object() || str(binary, "id") != id)
        throw std::runtime_error("Meta did not return the requested build.");
      return binary;
    } catch (const RequestError& error) {
      if (error.status < 500 || error.status >= 600 || stop_.stop_requested()) throw;
      if (attempt == 2) break;
      if (!pause_for(stop_, std::chrono::milliseconds(250 << attempt))) throw;
    }
  }
  // Meta Platform Utility's OVRBinaryMetadataQuery avoids the all-assets
  // connection that fails for large catalogs, but has no asset files at all.
  // The plan is marked as lacking its asset catalog: base installs from it
  // play from the APK and OBB with a launcher notice, it never replaces an
  // installed catalog, and add-on and asset jobs refuse it until a full query
  // succeeds.
  auto metadata = [&](std::string_view binary_id, std::string_view type) {
    auto response = query("9973165429396704", {{"binary_id", binary_id}});
    auto value = field(field(response, "data"), "metadata");
    if (!value.is_object() || str(value, "id") != binary_id || str(value, "__typename") != type)
      throw std::runtime_error("Meta did not return the requested binary metadata.");
    return value;
  };
  auto binary = metadata(id, "AndroidBinary");
  auto obb_id = str(field(binary, "obb_binary"), "id");
  if (!obb_id.empty()) {
    require_id(obb_id);
    binary["obb_binary"] = metadata(obb_id, "OBBBinary");
  }
  binary["asset_files_unavailable"] = true;
  try {
    binary["asset_files"] = fallback_assets(id);
  } catch (...) {
    if (stop_.stop_requested())
      throw;
    log::warn("meta", "Meta's reduced asset list for build {} is unavailable.", id);
  }
  return binary;
}
Json Store::fallback_assets(std::string_view id) const {
  // Meta Platform Utility's binary details query lists every asset file's id
  // and size where the full query fails, but no names: the delivery server
  // names each file. An asset id's name never changes, so names are cached.
  static std::mutex cache_mutex;
  static std::map<std::string, std::string> cache;
  auto response = query("24072064135771905", {{"binaryID", id}});
  auto &binary = field(field(response, "data"), "node");
  if (str(binary, "id") != id)
    throw std::runtime_error("Meta did not return the requested build.");
  auto &assets = field(binary, "asset_files");
  auto list = nodes(assets);
  if (number(assets, "count") > list.size() ||
      field(field(assets, "page_info"), "has_next_page") == true)
    throw std::runtime_error("Meta's asset list was incomplete.");
  struct Entry {
    std::string id, name;
    uint64_t size;
  };
  std::vector<Entry> entries;
  for (auto &a : list) {
    Entry e{str(a, "id"), str(a, "file_name"), number(a, "size")};
    if (!digits(e.id) || !e.size)
      throw std::runtime_error("Meta returned invalid asset details.");
    std::lock_guard lock(cache_mutex);
    if (auto i = cache.find(e.id); i != cache.end())
      e.name = i->second;
    entries.push_back(std::move(e));
  }
  // The delivery credential is fetched once, before the workers share it.
  if (std::ranges::any_of(entries, [](auto &e) { return e.name.empty(); }))
    download_url(File{entries.front().id});
  std::atomic<std::size_t> next{0};
  auto worker = [&] {
    for (std::size_t i; (i = next++) < entries.size() && !stop_.stop_requested();) {
      auto &e = entries[i];
      if (!e.name.empty())
        continue;
      try {
        auto url = download_url(File{e.id});
        e.name = names_ ? names_(url) : http::delivery_file_name(url, stop_);
      } catch (...) {
      }
      if (!paths::safe_name(e.name)) {
        e.name.clear();
        continue;
      }
      std::lock_guard lock(cache_mutex);
      cache[e.id] = e.name;
    }
  };
  {
    std::vector<std::jthread> workers;
    for (int i = 0; i < 16; ++i)
      workers.emplace_back(worker);
  }
  if (stop_.stop_requested())
    throw std::runtime_error("Cancelled.");
  Json out = Json::array();
  std::set<std::string> names;
  for (auto &e : entries) {
    auto key = e.name;
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    if (!paths::safe_name(e.name) || !names.insert(key).second) {
      log::warn("meta", "Asset {} of build {} has no usable file name; it is not listed.", e.id, id);
      continue;
    }
    out.push_back({{"id", e.id}, {"file_name", e.name}, {"size", e.size}});
  }
  return Json{{"nodes", out}};
}

Details Store::details(std::string_view id) const {
  require_id(id);
  auto j = query("6549406941839522", {{"itemId", id}, {"hmdType", "EUREKA"}});
  auto &item = field(field(j, "data"), "item");
  if (str(item, "id") != id || !quest_app(item))
    throw std::runtime_error("Meta did not return a live Quest listing.");
  auto &b = field(item, "latest_supported_binary");
  Details result{card(item), str(b, "id"), str(b, "version"),
                 str(b, "version_code"), number(b, "size")};
  if (digits(result.live_id)) {
    auto binary = b;
    binary.update(binary_details(result.live_id));
    result.plan = build_plan(id, binary);
  }
  return result;
}
Json Store::public_item(std::string_view id) const {
  require_id(id);
  if (listing_)
    return listing_(id);
  // The public storefront now requires its anonymous CSRF verifier in both
  // the form and X-FB-LSD header. This is fetched without account credentials;
  // the account's access token is never sent to www.meta.com.
  thread_local std::string verifier;
  Json j;
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (verifier.empty()) {
      auto html = http::get_public(
          "https://www.meta.com/experiences/" + std::string(id) + "/", stop_);
      std::smatch match;
      const std::regex pattern(
          R"venture(\["LSD",\[\],\{"token":"([A-Za-z0-9_-]{1,200})")venture");
      if (!std::regex_search(html, match, pattern))
        throw std::runtime_error(
            "Public Meta artwork is unavailable. Refresh to retry.");
      verifier = match[1].str();
    }
    try {
      j = post(
          "https://www.meta.com/ocapi/graphql",
          {{"doc_id", "6549406941839522"},
           {"lsd", verifier},
           {"variables", Json{{"itemId", id}, {"hmdType", "MONTEREY"}}.dump()}},
          stop_, {{"X-FB-LSD", verifier}});
      break;
    } catch (...) {
      verifier.clear();
      if (attempt || stop_.stop_requested())
        throw;
    }
  }
  auto &item = field(field(j, "data"), "item");
  if (str(item, "id") != id)
    throw std::runtime_error("Artwork listing is unavailable.");
  return item;
}
Game Store::artwork(std::string_view id) const { return card(public_item(id)); }
std::string cover_from_page(std::string_view html) {
  // JSON-LD: "primaryImageOfPage":{"@id":"https:\/\/..."}. Scanned by hand:
  // store pages are large and std::regex recursion is unsafe on them.
  if (auto at = html.find("\"primaryImageOfPage\""); at != html.npos) {
    auto id = html.find("\"@id\"", at);
    auto open = id == html.npos ? id : html.find('"', html.find(':', id + 5));
    if (open != html.npos && id - at < 64) {
      size_t close = open + 1;
      while (close < html.size() && html[close] != '"')
        close += html[close] == '\\' ? 2 : 1;
      try {
        auto url = Json::parse(html.substr(open, close - open + 1)).get<std::string>();
        if (http::allowed_delivery(url))
          return url;
      } catch (...) {
      }
    }
  }
  if (auto at = html.find("property=\"og:image\""); at != html.npos) {
    auto tag = html.rfind('<', at), end = html.find('>', at);
    auto content = html.find("content=\"", tag);
    if (tag != html.npos && content != html.npos && content < end) {
      content += 9;
      std::string url(html.substr(content, html.find('"', content) - content));
      for (size_t amp; (amp = url.find("&amp;")) != std::string::npos;)
        url.replace(amp, 5, "&");
      if (http::allowed_delivery(url))
        return url;
    }
  }
  return {};
}
std::string Store::cover(std::string_view id) const {
  require_id(id);
  return cover_from_page(http::get_public(
      "https://www.meta.com/experiences/" + std::string(id) + "/", stop_));
}
Json Store::builds(std::string_view id) const {
  require_id(id);
  const auto response = query("2885322071572384", {{"applicationID", id}});
  const auto &application = field(field(response, "data"), "node");
  Json candidates = Json::array();
  for (auto key : {"primary_binaries", "binaries"}) {
    if (field(application, key).is_null())
      continue;
    candidates = nodes(application[key]);
    if (!candidates.empty())
      break;
  }
  // A version index keeps ties in response order and places newest uploads first.
  std::multimap<uint64_t, Json, std::greater<uint64_t>> versions;
  for (const auto &candidate : candidates)
    if (android_build(candidate))
      versions.emplace(build_revision(candidate), candidate);
  Json menu = Json::array();
  for (const auto &[revision, candidate] : versions)
    menu.push_back(candidate);
  return menu;
}
Snapshot Store::owned_listing(std::string_view id) const {
  require_id(id);
  const auto response = query("6549406941839522", {{"itemId", id}, {"hmdType", "EUREKA"}});
  const auto &data = field(response, "data");
  const auto &item = field(data, "item");
  if (str(item, "id") != id || !quest_app(item))
    throw std::runtime_error("Meta has no Quest game at that link.");
  const auto &entitled = field(item, "is_viewer_entitled");
  Snapshot s;
  s.account_id = str(field(field(data, "viewer"), "user"), "id");
  if (!entitled.is_boolean() || !digits(s.account_id))
    throw std::runtime_error("Meta did not say whether this account owns the game. Try again later.");
  if (entitled.get<bool>()) {
    auto g = card(item);
    g.owned = true;
    s.games.push_back(std::move(g));
  }
  return s;
}
bool Store::owns(std::string_view id) const {
  const auto response = query("6549406941839522", {{"itemId", id}, {"hmdType", "EUREKA"}});
  const auto &item = field(field(response, "data"), "item");
  if (const auto &entitled = field(item, "is_viewer_entitled"); str(item, "id") == id && entitled.is_boolean())
    return entitled.get<bool>();
  const auto entitlement = library();
  return std::ranges::find(entitlement.games, id, &Game::id) != entitlement.games.end();
}
Plan Store::owned_plan(std::string_view id) const {
  require_id(id);
  // The live headset release is independent of the upload/version listing.
  const auto release = query("6549406941839522", {{"hmdType", "EUREKA"}, {"itemId", id}});
  const auto &application = field(field(release, "data"), "item");
  if (str(application, "id") != id || !quest_app(application))
    throw std::runtime_error("The live release response did not identify this Quest application.");
  // The listing says whether this account owns the app; the whole library is
  // asked only when it does not (Meta fails that query for some accounts).
  if (const auto &entitled = field(application, "is_viewer_entitled"); entitled.is_boolean()) {
    if (!entitled.get<bool>())
      throw std::runtime_error("Meta says this account does not own this game.");
  } else {
    const auto entitlement = library();
    if (std::ranges::find(entitlement.games, id, &Game::id) == entitlement.games.end())
      throw std::runtime_error("Refresh your library: ownership could not be verified for this download.");
  }
  auto selected = field(application, "latest_supported_binary");
  const auto selected_id = str(selected, "id");
  if (!digits(selected_id) || !android_build(selected))
    throw std::runtime_error("No downloadable live Quest release was returned.");
  const auto details = binary_details(selected_id);
  selected.update(details);
  return build_plan(id, selected);
}
std::string Store::download_url(const File &f) const {
  const auto address = [&] {
    if (!f.uri.empty())
      return f.uri;
    if (!digits(f.id))
      throw std::runtime_error("Download content must have a numeric identifier.");
    return "https://securecdn.oculus.com/binaries/download/?" + http::form({{"id", f.id}});
  }();
  if (!http::allowed_delivery(address))
    throw std::runtime_error("Download destination is not an approved Meta host.");
  const auto destination = http::parse_url(address);
  if (token_endpoint(destination)) {
    if (delivery_.empty()) {
      const http::Fields scope{{"access_token", token_}, {"app_id", "1481000308606657"}};
      const auto authorization = request(
          "https://graph.oculus.com/authenticate_application?" + http::form(scope), {});
      const auto credential = str(authorization, "access_token");
      if (credential.empty())
        throw std::runtime_error("Meta supplied no authorization for Quest file delivery.");
      delivery_ = credential;
    }
    http::Fields parameters;
    for (const auto &parameter : http::query(destination.query))
      if (parameter.first != "access_token")
        parameters.push_back(parameter);
    parameters.emplace_back("access_token", delivery_);
    const auto fragment = address.find('#');
    return address.substr(0, address.find_first_of("?#")) + "?" + http::form(parameters) +
           (fragment == address.npos ? std::string() : address.substr(fragment));
  }
  return address;
}
} // namespace venture::meta
