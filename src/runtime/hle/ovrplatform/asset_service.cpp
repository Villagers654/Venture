#include "hle/ovrplatform/asset_service.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <condition_variable>

#include "common/download/asset_files.h"
#include "common/download/unity_bundle.h"
#include "common/log.h"
#include "common/saves_migration.h"

namespace venture::ovrplatform {
namespace {
bool gated(const meta::File& f) { return !f.sku.empty() || !f.iap_id.empty(); }
Id id(const meta::File& f) {
  Id value = 0;
  std::from_chars(f.id.data(), f.id.data() + f.id.size(), value);
  return value;
}
Result completed(const meta::File& f, std::string path) {
  Result r;
  r.model = {"AssetFileDownloadResult",
             {{"AssetId", id(f)}, {"Filepath", std::move(path)}}};
  return r;
}
Error requires_launcher() {
  return {ErrorCode::unavailable, 0,
          "Venture: asset download requires launcher"};
}
Error unknown_asset() {
  return {ErrorCode::not_entitled, 0,
          "Venture: asset is unknown or unavailable to this account"};
}
// A language pack serves its exact tag first, else any tag of its language:
// packs named for a language ("ko") serve the system's regional tag ("ko-KR").
std::vector<meta::File>::iterator find_language(std::vector<meta::File>& files, std::string_view tag) {
  auto lower = [](std::string_view text, bool primary) {
    std::string out(primary ? text.substr(0, text.find_first_of("-_")) : text);
    for (auto& c : out) c = c == '_' ? '-' : char(std::tolower(static_cast<unsigned char>(c)));
    return out;
  };
  for (bool primary : {false, true}) {
    auto want = lower(tag, primary);
    auto found = std::ranges::find_if(files, [&](const meta::File& f) {
      return !f.language_tag.empty() && lower(f.language_tag, primary) == want;
    });
    if (found != files.end() || want.empty()) return found;
  }
  return files.end();
}
Model update(const meta::File& f, std::uint64_t done, bool complete) {
  // OVR_AssetFileDownloadUpdate.h: deprecated counters are uint32/int32;
  // *Long counters are uint64/int64, on both guest ABIs.
  return {"AssetFileDownloadUpdate",
          {{"AssetId", id(f)},
           {"AssetFileId", id(f)},
           {"BytesTotal", static_cast<std::uint32_t>(f.size)},
           {"BytesTotalLong", f.size},
           {"BytesTransferred", static_cast<std::int32_t>(done)},
           {"BytesTransferredLong", static_cast<std::int64_t>(done)},
           {"Completed", complete}}};
}
}  // namespace
AssetService::AssetService(AppIdentity app, std::filesystem::path install,
                           std::string guest_root,
                           AssetOwnershipReader ownership,
                           std::shared_ptr<PipeChannel> channel,
                           std::filesystem::path language_state,
                           std::filesystem::path obb_view)
    : app_(std::move(app)),
      install_(std::move(install)),
      language_state_(std::move(language_state)),
      obb_view_(std::move(obb_view)),
      guest_root_(std::move(guest_root)),
      ownership_(std::move(ownership)),
      channel_(std::move(channel)) {
  // A bundle cut short before it reached this PC fails only when the game
  // loads it, so name it up front for the log and debug report.
  for (const auto& f : catalog().files) {
    const auto file = install_ / paths::from_utf8(f.name);
    std::error_code ec;
    if (!paths::within(install_, file) || !std::filesystem::is_regular_file(file, ec)) continue;
    const auto size = std::filesystem::file_size(file, ec);
    if (const auto whole = download::unity_bundle_size(file); !ec && whole && *whole != size)
      log::error("facts", "Game file {} is damaged: {} bytes of {}. Install or import the game again.", f.name,
                 size, *whole);
  }
  if (!language_state_.empty() && std::filesystem::exists(language_state_))
    language_ = str(paths::read_json(language_state_), "tag");
}
meta::Plan AssetService::catalog() const {
  if (!paths::within(install_, install_ / "plan.json"))
    throw std::runtime_error("Unsafe asset catalog");
  auto p = meta::parse_plan(paths::read_json(install_ / "plan.json"));
  // An import Meta does not know is keyed by its package, with no app id.
  const auto app_id = p.local && app_.app_id.empty() ? p.package : app_.app_id;
  if (p.app_id != app_id || p.binary_id != app_.binary_id ||
      p.package != app_.package ||
      p.version_code != std::to_string(app_.version_code))
    throw std::runtime_error("Asset catalog identity changed");
  reduced_ = !p.assets_complete;
  // Files an import brought have no Meta asset id. Games still look them up
  // by name (Batman's scene bundles), so each gets a stable id of its own:
  // FNV-1a of its lowercase name with the top bit set, which Meta's ids
  // (below 2^63) never use. The OBB view publishes them like store assets.
  for (auto& f : p.files)
    if (f.kind == "asset" && f.id.empty()) {
      std::uint64_t h = 14695981039346656037ull;
      for (unsigned char c : f.name) h = (h ^ std::uint64_t(std::tolower(c))) * 1099511628211ull;
      f.id = std::to_string(h | (1ull << 63));
    }
  for (auto& f : p.files)
    if (f.kind == "asset" &&
        (!valid_id(f.id) || !f.size || f.size > INT64_MAX ||
         (!f.asset_type.empty() && f.asset_type != "default" &&
          f.asset_type != "store" && f.asset_type != "language_pack")))
      throw std::runtime_error("Invalid asset catalog");
  return p;
}
std::vector<meta::File> AssetService::allowed(std::stop_token stop) const {
  auto p = catalog();
  AssetOwnership owned;
  if (std::ranges::any_of(
          p.files, [](const auto& f) { return f.kind == "asset" && gated(f); }))
    owned = ownership_ ? ownership_(stop)
                       : AssetOwnership{failure(ErrorCode::unavailable)};
  // Failed purchase reads hide gated content; base assets remain useful
  // offline.
  std::erase_if(p.files, [&](const auto& f) {
    return f.kind != "asset" || (gated(f) && (owned.error || f.sku.empty() ||
                                              !owned.skus.contains(f.sku)));
  });
  // Store bundles can alias the same content ID under different filenames.
  // ID selectors/list entries prefer an installed, owned alias; name selectors
  // still address the exact catalog filename.
  std::stable_partition(p.files.begin(), p.files.end(),
                        [&](const auto& f) { return installed(f); });
  return p.files;
}
bool AssetService::installed(const meta::File& f) const {
  auto p = install_ / paths::from_utf8(f.name);
  return paths::within(install_, p) && std::filesystem::is_regular_file(p) &&
         std::filesystem::file_size(p) == f.size;
}
std::string AssetService::filepath(const meta::File& f) const {
  // Publish (or remove) the view before returning the path to either SDK.
  // Downloads replace source files, so existing aliases must be reconciled.
  if (!obb_view_.empty()) publish_obb_file(install_ / paths::from_utf8(f.name), obb_view_);
  return guest_root_ + '/' + f.name;
}
Json AssetService::command(std::string_view action, const meta::File& f,
                           std::stop_token stop) const {
  if (!channel_ || stop.stop_requested()) return Json{};
  return channel_->call({{"op", "asset." + std::string(action)},
                         {"id", f.id}, {"name", f.name}});
}
Model AssetService::details(const meta::File& f, std::stop_token stop) const {
  auto status =
      installed(f) ? std::string("installed") : std::string("available");
  if (channel_ && status != "installed") {
    auto reply = command("status", f, stop);
    auto state = field(reply, "state");
    if (field(reply, "ok") == true && state.is_number_integer() &&
        state.get<int>() >= int(download::State::queued) &&
        state.get<int>() <= int(download::State::downloading))
      status = "in-progress";
  }
  // OVR_AssetDetails.h: installed/available/in-progress; free/entitled;
  // default/store/language_pack. Never infer language from a filename.
  auto type = f.asset_type.empty()
                  ? (f.language_tag.empty() ? "default" : "language_pack")
                  : f.asset_type;
  Model m{"AssetDetails",
          {{"AssetId", id(f)},
           {"AssetType", type},
           {"DownloadStatus", status},
           {"Filepath", status == "installed" ? filepath(f) : std::string{}},
           // Without Meta's full catalog an asset's add-on is unknown; the game
           // still checks the account's real purchases before offering paid content.
           {"IapStatus", std::string(gated(f) || reduced_ ? "entitled" : "free")},
           {"Metadata", f.metadata}}};
  if (!f.language_tag.empty())
    m.fields["Language"] = std::make_shared<const Model>(
        Model{"LanguagePackInfo",
              {{"Tag", f.language_tag},
               {"EnglishName", f.language_english_name},
               {"NativeName", f.language_native_name}}});
  return m;
}
Result AssetService::download(const meta::File& f, std::stop_token stop,
                              Notify notify) {
  if (installed(f)) {
    auto path = filepath(f);
    if (notify) notify(update(f, f.size, true));
    return completed(f, std::move(path));
  }
  if (!channel_) return {requires_launcher()};
  auto reply = command("download", f, stop);
  if (stop.stop_requested()) return {failure(ErrorCode::cancelled)};
  if (field(reply, "ok") != true) return {failure(ErrorCode::unavailable)};
  if (notify) notify(update(f, 0, false));
  Result r;
  r.deferred = [self = shared_from_this(), f,
                generation = number(reply, "generation"),
                notify = std::move(notify)](std::stop_token wait_stop) {
    return self->await_download(f, generation, wait_stop, notify);
  };
  return r;
}
Result AssetService::await_download(meta::File f, std::uint64_t generation,
                                    std::stop_token stop, Notify notify) {
  std::uint64_t last = 0;  // The enqueue path already notified zero progress.
  std::mutex mutex;
  std::condition_variable_any wake;
  for (;;) {
    if (stop.stop_requested()) return {failure(ErrorCode::cancelled)};
    // Recheck account identity and purchases before exposing a completed file.
    auto files = allowed(stop);
    if (std::ranges::none_of(files, [&](const auto& x) {
          return x.id == f.id && x.name == f.name;
        }))
      return {unknown_asset()};
    auto reply = command("status", f, stop);
    if (stop.stop_requested()) return {failure(ErrorCode::cancelled)};
    if (field(reply, "ok") != true ||
        !field(reply, "state").is_number_integer())
      return {failure(ErrorCode::unavailable)};
    if (number(reply, "generation") != generation)
      return {failure(ErrorCode::cancelled)};
    int state = reply["state"].get<int>();
    auto done = std::min(number(reply, "done"), f.size);
    if (state == int(download::State::complete)) {
      if (!installed(f)) return {failure(ErrorCode::invalid_response)};
      auto path = filepath(f);
      if (notify) notify(update(f, f.size, true));
      return completed(f, std::move(path));
    }
    if (state == int(download::State::paused))
      return {failure(ErrorCode::cancelled)};
    if (state == int(download::State::failed) || state < 0)
      return {failure(ErrorCode::unavailable)};
    if (done != last && notify) notify(update(f, done, false));
    last = done;
    std::unique_lock lock(mutex);
    wake.wait_for(lock, stop, std::chrono::milliseconds(250),
                  [] { return false; });
  }
}
void AssetService::select_language(std::string_view tag) {
  std::lock_guard lock(mutex_);
  if (!language_state_.empty()) {
    auto root = language_state_.parent_path();
    if (!paths::within(root, language_state_) ||
        !paths::within(root,
                       paths::from_utf8(paths::utf8(language_state_) + ".tmp")))
      throw std::runtime_error("Unsafe language state");
    paths::atomic_write(language_state_, Json{{"tag", tag}}.dump());
  }
  language_ = tag;
}
Result AssetService::request(std::string_view op, const RequestArgs& args,
                             std::stop_token stop, Notify notify) {
  if (args.error) return {args.error};
  if (stop.stop_requested()) return {failure(ErrorCode::cancelled)};
  constexpr std::string_view operations[]{"GetList",
                                          "Status",
                                          "StatusById",
                                          "StatusByName",
                                          "Download",
                                          "DownloadById",
                                          "DownloadByName",
                                          "DownloadCancel",
                                          "DownloadCancelById",
                                          "DownloadCancelByName",
                                          "Delete",
                                          "DeleteById",
                                          "DeleteByName",
                                          "LanguageGetCurrent",
                                          "LanguageSetCurrent"};
  if (std::ranges::find(operations, op) == std::end(operations))
    return {failure(ErrorCode::unsupported)};
  if ((op == "GetList" || op == "LanguageGetCurrent") && !args.values.empty())
    return {failure(ErrorCode::invalid_argument)};
  try {
    auto files = allowed(stop);
    if (stop.stop_requested()) return {failure(ErrorCode::cancelled)};
    if (op == "GetList") {
      Result r;
      r.model.kind = "AssetDetailsArray";
      std::set<std::string> ids;
      std::size_t present = 0;
      std::string languages;
      for (auto& f : files)
        if (ids.insert(f.id).second) {
          r.model.elements.push_back(details(f, stop));
          present += installed(f);
          if (!f.language_tag.empty()) languages += (languages.empty() ? "" : ",") + f.language_tag;
        }
      log::info("facts", "Game asset list: {} files, {} installed; language packs: {}", ids.size(), present,
                languages.empty() ? "none" : languages);
      return r;
    }
    auto found = files.end();
    if (op == "LanguageGetCurrent") {
      std::string selected;
      {
        std::lock_guard lock(mutex_);
        selected = language_;
      }
      auto tag = selected.empty() ? host_locale() : selected;
      if (!tag.empty()) found = find_language(files, tag);
    } else if (args.values.size() == 1) {
      if (op == "LanguageSetCurrent") {
        auto tag = std::get_if<std::string>(&args.values[0]);
        if (tag && !tag->empty()) found = find_language(files, *tag);
        log::info("facts", "Game asked for language pack {}: {}", tag ? *tag : std::string("-"),
                  found == files.end() ? std::string("none in the asset list") : found->name);
      } else if (op.ends_with("ByName")) {
        if (auto name = std::get_if<std::string>(&args.values[0]);
            name && paths::safe_name(*name))
          found = std::ranges::find(files, *name, &meta::File::name);
      } else if (auto value = std::get_if<std::uint64_t>(&args.values[0]);
                 value && *value) {
        found =
            std::ranges::find(files, std::to_string(*value), &meta::File::id);
      }
    } else
      return {failure(ErrorCode::invalid_argument)};
    // Unknown and unowned selectors have the same response; no filename leaks.
    if (found == files.end()) return {unknown_asset()};
    auto f = *found;
    if (op.starts_with("Status") || op == "LanguageGetCurrent") {
      if (op == "LanguageGetCurrent" && !installed(f))
        return {failure(ErrorCode::unavailable)};
      Result r;
      r.model = details(f, stop);
      return r;
    }
    if (op == "LanguageSetCurrent") {
      auto r = download(f, stop, notify);
      if (r.deferred) {
        auto wait = std::move(r.deferred);
        r.deferred = [self = shared_from_this(), wait = std::move(wait),
                      tag = f.language_tag](std::stop_token s) {
          auto result = wait(s);
          if (!result.error) self->select_language(tag);
          return result;
        };
      } else if (!r.error)
        select_language(f.language_tag);
      return r;
    }
    const bool cancel = op.starts_with("DownloadCancel");
    if (op.starts_with("Download") && !cancel) return download(f, stop, notify);
    bool success = false;
    if (cancel) {
      if (channel_) {
        auto reply = command("cancel", f, stop);
        if (field(reply, "ok") != true)
          return {failure(ErrorCode::unavailable)};
        success = field(reply, "success") == true;
      }
    } else if (op.starts_with("Delete")) {
      std::lock_guard lock(mutex_);
      if (channel_) {
        auto reply = command("delete", f, stop);
        if (field(reply, "ok") != true)
          return {failure(ErrorCode::unavailable)};
        success = field(reply, "success") == true;
      } else {
        success = download::remove_asset(install_, app_.app_id, app_.binary_id,
                                         f.name);
      }
    } else
      return {failure(ErrorCode::unsupported)};
    Result r;
    r.model = {
        cancel ? "AssetFileDownloadCancelResult" : "AssetFileDeleteResult",
        {{"AssetId", id(f)},
         {"AssetFileId", id(f)},
         {"Filepath", filepath(f)},
         {"Success", success}}};
    return r;
  } catch (...) {
    return {failure(ErrorCode::unavailable)};
  }
}
AssetOwnershipReader asset_ownership_reader(
    std::shared_ptr<Provider> provider, std::shared_ptr<HttpTransport> http) {
  // Same already live-verified read as the launcher DLC pipeline (P7,
  // plan 2.4).
  http->register_endpoint({"asset_purchases",
                           HttpMethod::get,
                           "/application/viewer_purchases",
                           {"fields"},
                           HttpShape::graph_query,
                           true});
  struct Cache {
    std::mutex mutex;
    std::string token;
    AssetOwnership owned;
    std::chrono::steady_clock::time_point until{};
  };
  auto cache = std::make_shared<Cache>();
  return [provider = std::move(provider), http = std::move(http),
          cache](std::stop_token stop) -> AssetOwnership {
    auto auth = provider->access_token(
        stop);  // Checks the launch account even on cache hits.
    if (auth.error) return {auth.error};
    std::lock_guard lock(cache->mutex);
    if (cache->token == auth.text &&
        std::chrono::steady_clock::now() < cache->until)
      return cache->owned;
    AssetOwnership result;
    auto lifetime = std::chrono::seconds(60);
    try {
      HttpRequest request{"asset_purchases",
                          {{"fields",
                            "id,grant_time,expiration_time,item{sku,type},"
                            "developer_payload,reporting_id"}},
                          auth.text};
      Json rows = Json::array();
      std::set<std::string> visited;
      for (unsigned page = 0; page < 16; ++page) {
        auto response = http->send(request, stop);
        result.error = http_response_error(response);
        if (result.error) break;
        auto body = Json::parse(response.body, nullptr, false);
        const auto& data = field(body, "data");
        if (!data.is_array())
          throw std::runtime_error("Invalid asset purchases");
        for (auto& row : data) rows.push_back(row);
        const auto& next = field(field(body, "paging"), "next");
        if (next.is_null() || next == "") break;
        if (!next.is_string() ||
            !visited.insert(next.get<std::string>()).second || page == 15) {
          result.error = failure(ErrorCode::incomplete);
          break;
        }
        auto url = next.get<std::string>();
        auto parsed = venture::http::parse_url(url);
        if (!valid_next_page_url(url) ||
            parsed.path != "/v1.20/application/viewer_purchases")
          throw std::runtime_error("Invalid asset purchase page");
        for (auto& [key, value] : venture::http::query(parsed.query))
          if (key == "access_token" || key == "input_token")
            throw std::runtime_error("Credential-bearing asset page");
        // Paging URLs carry their own parameters; the transport validates the
        // origin, while this service additionally pins the edge and
        // credentials.
        request.parameters.clear();
        request.next_url = std::move(url);
      }
      if (!result.error) {
        result.skus = meta::parse_owned_skus(rows);
        auto now = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now());
        for (auto& row : rows) {
          auto expiry = number(row, "expiration_time");
          if (expiry && expiry > static_cast<std::uint64_t>(now))
            lifetime =
                std::min(lifetime, std::chrono::seconds(std::min<std::uint64_t>(
                                       60, expiry - now - 1)));
        }
      }
      auto current = provider->access_token(stop);
      if (current.error || current.text != auth.text)
        result = {current.error ? current.error
                                : failure(ErrorCode::identity_mismatch)};
    } catch (...) {
      result = {failure(ErrorCode::invalid_response)};
    }
    if (stop.stop_requested()) result = {failure(ErrorCode::cancelled)};
    if (result.error)
      log::warn("ovrplatform", "Asset ownership read failed error={}",
                static_cast<int>(result.error.code));
    cache->token = auth.text;
    cache->owned = result;
    cache->until = std::chrono::steady_clock::now() + lifetime;
    return result;
  };
}
void register_asset_handlers(Session& session,
                             std::shared_ptr<AssetService> service) {
  for (auto& spec : request_specs()) {
    std::string_view name = spec.name;
    bool asset = name.starts_with("ovr_AssetFile_");
    if (!asset && !name.starts_with("ovr_LanguagePack_")) continue;
    auto op = asset                          ? std::string(name.substr(14))
              : name.ends_with("GetCurrent") ? "LanguageGetCurrent"
                                             : "LanguageSetCurrent";
    auto kind = op == "GetList" ? "AssetDetailsArray"
                : op.starts_with("Status") || op == "LanguageGetCurrent"
                    ? "AssetDetails"
                : op.starts_with("DownloadCancel")
                    ? "AssetFileDownloadCancelResult"
                : op.starts_with("Delete") ? "AssetFileDeleteResult"
                                           : "AssetFileDownloadResult";
    session.register_handler(
        name, {[service, op, &session](Provider&, const RequestArgs& args,
                                       std::stop_token stop) {
                 return service->request(op, args, stop, [&session](Model m) {
                   session.notify(kAssetDownloadUpdate, std::move(m));
                 });
               },
               kind});
  }
}
}  // namespace venture::ovrplatform
