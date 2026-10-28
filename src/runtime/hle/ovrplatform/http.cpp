#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>

#include "common/http/client.h"
#include "common/log.h"
#include "hle/ovrplatform/provider.h"
#include "hle/ovrplatform/identity_iap.h"
#include "hle/ovrplatform/social.h"
#include "hle/ovrplatform/progression.h"

namespace venture::ovrplatform {
namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
struct Transfer {
  std::string body;
  std::stop_token stop;
};
std::size_t receive(char* data, std::size_t size, std::size_t count, void* user) {
  auto& transfer = *static_cast<Transfer*>(user);
  if (size && count > std::numeric_limits<std::size_t>::max() / size) return 0;
  auto bytes = size * count;
  if (bytes > 4 * 1024 * 1024 - transfer.body.size()) return 0;
  try {
    transfer.body.append(data, bytes);
  } catch (...) {
    return 0;
  }
  return bytes;
}
int progress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  return static_cast<Transfer*>(user)->stop.stop_requested() ? 1 : 0;
}
bool clean(std::string_view text) {
  return std::none_of(text.begin(), text.end(),
                      [](unsigned char c) { return c <= 32 || c == 127 || c == '\\'; });
}
bool transient(std::int32_t status) { return status == 429 || (status >= 500 && status <= 599); }
bool pause_until(Clock::time_point deadline, std::stop_token stop) {
  std::mutex mutex;
  std::condition_variable_any wake;
  std::unique_lock lock(mutex);
  wake.wait_until(lock, stop, deadline, [] { return false; });
  return !stop.stop_requested();
}
// Graph errors Meta marks retryable: is_transient, or its documented temporary
// (1, 2) and rate-limit (4, 17, 32, 341, 613) codes.
bool graph_transient(const Json& body) {
  auto retryable = [](const Json& e) {
    if (!e.is_object()) return false;
    if (e.contains("is_transient") && e["is_transient"].is_boolean() && e["is_transient"].get<bool>())
      return true;
    if (!e.contains("code") || !e["code"].is_number_integer()) return false;
    switch (e["code"].get<std::int64_t>()) {
      case 1: case 2: case 4: case 17: case 32: case 341: case 613: return true;
      default: return false;
    }
  };
  if (!body.is_object()) return false;
  if (body.contains("error") && retryable(body["error"])) return true;
  if (body.contains("errors") && body["errors"].is_array())
    for (auto& e : body["errors"])
      if (retryable(e)) return true;
  return false;
}
// The first Graph error's numeric fields for logs: never its message or any token.
std::string graph_error_codes(const Json& body) {
  const Json* e = nullptr;
  if (body.is_object() && body.contains("error") && body["error"].is_object())
    e = &body["error"];
  else if (body.is_object() && body.contains("errors") && body["errors"].is_array() &&
           !body["errors"].empty() && body["errors"][0].is_object())
    e = &body["errors"][0];
  if (!e) return "none";
  std::string text;
  for (auto key : {"code", "error_subcode", "api_error_code", "is_transient"})
    if (e->contains(key) && ((*e)[key].is_number_integer() || (*e)[key].is_boolean()))
      text += std::string(text.empty() ? "" : " ") + key + "=" + (*e)[key].dump();
  return text.empty() ? "unnumbered" : text;
}
bool graph_190(const Json& body) {
  auto has_code = [](const Json& e) {
    return e.is_object() && e.contains("code") && e["code"].is_number_integer() && e["code"] == 190;
  };
  if (!body.is_object()) return false;
  if (body.contains("error") && has_code(body["error"])) return true;
  if (body.contains("errors") && body["errors"].is_array())
    for (auto& e : body["errors"])
      if (has_code(e)) return true;
  return false;
}
class CurlTransport final : public HttpTransport {
 public:
  explicit CurlTransport(std::string base) : base_(std::move(base)) {
    register_endpoint({"presence.heartbeat", HttpMethod::post, "/user_heartbeat",
        {"current_status", "in_vr", "push_token", "rich_presence_options", "app_id_override"},
        HttpShape::unversioned_query});
    // P0 enables only the previously verified edges. Feature packets explicitly
    // register new endpoints after their loopback and bounded live-read checks.
    register_endpoint({"/graphql",
                       HttpMethod::post,
                       "/graphql",
                       {"access_token", "doc_id", "variables"},
                       HttpShape::legacy_form,
                       true});
    // The app token exchange changes nothing server side, so it retries like a read. Meta has answered it 500
    // (graph code 12056) for a minute at a time while the same account's next launch succeeded; one failure
    // leaves the game without an identity and it quits on its entitlement check.
    register_endpoint({"/authenticate_application",
                       HttpMethod::post,
                       "/authenticate_application",
                       {"access_token", "app_id"},
                       HttpShape::legacy_form,
                       true});
    register_endpoint(
        {"/me", HttpMethod::get, "/me", {"access_token", "fields"}, HttpShape::legacy_form, true});
    register_endpoint({"/debug_token",
                       HttpMethod::get,
                       "/debug_token",
                       {"access_token", "input_token"},
                       HttpShape::legacy_form,
                       true});
    register_endpoint({"/user_nonce_generate",
                       HttpMethod::post,
                       "/user_nonce_generate",
                       {"access_token"},
                       HttpShape::legacy_form});
  }
  void register_endpoint(Endpoint e) override {
    if (e.name.empty() || e.path.empty() || e.path.front() != '/' || !clean(e.path) ||
        e.path.find_first_of("?#%") != e.path.npos || e.path.find("..") != e.path.npos ||
        e.path.find("//") != e.path.npos)
      throw std::invalid_argument("Invalid Platform endpoint");
    auto path = e.path;
    for (auto marker : {"{app}", "{id}"}) {
      auto at = path.find(marker);
      if (at != path.npos) path.erase(at, std::string_view(marker).size());
    }
    if (path.find_first_of("{}") != path.npos || (e.method == HttpMethod::erase && e.read_only))
      throw std::invalid_argument("Invalid Platform path template or retry policy");
    std::lock_guard lock(mutex_);
    const auto name = e.name;
    if (!endpoints_.emplace(name, std::move(e)).second)
      throw std::invalid_argument("Duplicate Platform endpoint");
  }
  HttpResponse post(std::string_view path, const Fields& fields, std::stop_token stop) override {
    return legacy(path, fields, stop, HttpMethod::post);
  }
  HttpResponse get(std::string_view path, const Fields& fields, std::stop_token stop) override {
    return legacy(path, fields, stop, HttpMethod::get);
  }
  HttpResponse send(const HttpRequest& r, std::stop_token stop) override {
    Endpoint e;
    {
      std::lock_guard lock(mutex_);
      auto it = endpoints_.find(r.endpoint);
      if (it == endpoints_.end()) return {0, {}, failure(ErrorCode::invalid_argument)};
      e = it->second;
    }
    if (r.bearer.find_first_of("\r\n") != r.bearer.npos || r.bearer.find('\0') != r.bearer.npos)
      return {0, {}, failure(ErrorCode::invalid_argument)};
    Fields fields = r.parameters;
    std::vector<std::string> seen;
    for (const auto& [key, value] : fields) {
      (void)value;
      if (std::find(e.parameters.begin(), e.parameters.end(), key) == e.parameters.end() ||
          std::find(seen.begin(), seen.end(), key) != seen.end() ||
          (e.shape != HttpShape::legacy_form &&
           (key == "access_token" || key == "input_token" || key == "app_version")))
        return {0, {}, failure(ErrorCode::invalid_argument)};
      seen.push_back(key);
    }
    auto path = e.path;
    for (auto [marker, id] : {std::pair{std::string_view("{app}"), std::string_view(r.app_id)},
                              {std::string_view("{id}"), std::string_view(r.id)}}) {
      auto at = path.find(marker);
      if (at != path.npos) {
        if (!valid_id(id)) return {0, {}, failure(ErrorCode::invalid_argument)};
        path.replace(at, marker.size(), id);
      }
    }
    std::string url;
    if (!r.next_url.empty()) {
      if (e.shape != HttpShape::graph_query || e.method != HttpMethod::get || !fields.empty() ||
          !valid_next_page_url(r.next_url))
        return {0, {}, failure(ErrorCode::invalid_argument)};
      // Loopback tests validate the same Meta origin before routing to their
      // server. A guest-supplied loopback URL can never receive credentials.
      url = base_ + r.next_url.substr(r.next_url.find('/', 8));
      if (url.find("?app_version=") == url.npos && url.find("&app_version=") == url.npos)
        fields.emplace_back("app_version", "0");
    } else {
      url = base_ + (e.shape == HttpShape::graph_query ? "/v1.20" : "") + path;
      if (e.shape == HttpShape::graph_query) fields.emplace_back("app_version", "0");
    }
    const bool retry = r.retry_reads && (e.read_only || e.method == HttpMethod::get);
    HttpResponse response;
    // Five attempts back off 0.25+0.5+1+2 s: a Quest app quits when its entitlement read
    // fails, and Meta's edge has dropped reads for ~2 s at a time (Batman, AC Nexus).
    unsigned attempt = 0;
    for (; attempt < (retry ? 5u : 1u); ++attempt) {
      Clock::time_point until;
      {
        std::lock_guard lock(mutex_);
        until = backoff_[r.endpoint].until;
      }
      if (!pause_until(until, stop)) return {0, {}, failure(ErrorCode::cancelled)};
      response = perform(url, fields, r.bearer, e.method,
                         e.shape == HttpShape::legacy_form && e.method == HttpMethod::post, stop);
      // A failed connection (no HTTP status) is as transient as a 5xx: a dropped read
      // must not fail an entitlement check that makes the game quit.
      const bool network = response.status == 0 && response.error.code == ErrorCode::unavailable;
      const auto body = Json::parse(response.body, nullptr, false);
      if ((!transient(response.status) && !network && !graph_transient(body)) ||
          response.error.code == ErrorCode::cancelled || graph_190(body))
        break;
      {
        std::lock_guard lock(mutex_);
        auto& b = backoff_[r.endpoint];
        b.failures = std::min(b.failures + 1, 5u);
        b.until = Clock::now() + std::chrono::milliseconds(250 * (1u << (b.failures - 1)));
      }
      log::debug("ovrplatform", "HTTP backoff status={} attempt={}", response.status, attempt + 1);
    }
    if (!transient(response.status)) {
      std::lock_guard lock(mutex_);
      backoff_.erase(r.endpoint);
    }
    if (retry && attempt == 5u)
      log::warn("ovrplatform", "HTTP read endpoint={} failed after {} attempts status={}", r.endpoint,
                attempt, response.status);
    response.error = http_response_error(response);
    if (response.error && response.error.code != ErrorCode::cancelled)
      log::warn("ovrplatform", "HTTP endpoint={} failed status={} attempts={} graph_error={}", r.endpoint,
                response.status, std::min(attempt + 1, 5u),
                graph_error_codes(Json::parse(response.body, nullptr, false)));
    return response;
  }

 private:
  HttpResponse legacy(std::string_view path, const Fields& fields, std::stop_token stop,
                      HttpMethod method) {
    {
      std::lock_guard lock(mutex_);
      auto it = endpoints_.find(path);
      if (it == endpoints_.end() || it->second.method != method ||
          it->second.shape != HttpShape::legacy_form)
        return {0, {}, failure(ErrorCode::invalid_argument)};
    }
    return send({std::string(path), fields}, stop);
  }
  HttpResponse perform(std::string url, const Fields& fields, const std::string& bearer,
                       HttpMethod method, bool form_body, std::stop_token stop) {
    static const auto initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (initialized != CURLE_OK) return {0, {}, failure(ErrorCode::unavailable)};
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) return {0, {}, failure(ErrorCode::unavailable)};
    std::string form;
    for (const auto& [key, value] : fields) {
      if (key.size() > INT_MAX || value.size() > INT_MAX)
        return {0, {}, failure(ErrorCode::invalid_argument)};
      std::unique_ptr<char, decltype(&curl_free)> k(
          curl_easy_escape(curl.get(), key.data(), int(key.size())), curl_free);
      std::unique_ptr<char, decltype(&curl_free)> v(
          curl_easy_escape(curl.get(), value.data(), int(value.size())), curl_free);
      if (!k || !v) return {0, {}, failure(ErrorCode::unavailable)};
      if (!form.empty()) form += '&';
      form += k.get();
      form += '=';
      form += v.get();
    }
    if (!form_body && !form.empty()) url += (url.find('?') == url.npos ? '?' : '&') + form;
    Transfer transfer{{}, stop};
    curl_slist* raw = nullptr;
    if (!bearer.empty()) {
      raw = curl_slist_append(nullptr, ("Authorization: Bearer " + bearer).c_str());
      if (!raw) return {0, {}, failure(ErrorCode::unavailable)};
    }
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(raw, curl_slist_free_all);
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR,
                     base_.starts_with("https:") ? "https" : "http");
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    http::use_system_ca(curl.get());
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, 15000L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    if (method == HttpMethod::post) {
      curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, form_body ? form.data() : "");
      curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE,
                       form_body ? static_cast<curl_off_t>(form.size()) : curl_off_t{0});
    } else if (method == HttpMethod::erase)
      curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &transfer);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    if (base_.starts_with("http:")) curl_easy_setopt(curl.get(), CURLOPT_NOPROXY, "*");
    auto result = curl_easy_perform(curl.get());
    long status = 0;  // libcurl host ABI only
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    if (result != CURLE_OK)
      return {static_cast<std::int32_t>(status),
              {},
              failure(stop.stop_requested() ? ErrorCode::cancelled : ErrorCode::unavailable)};
    return {static_cast<std::int32_t>(status), std::move(transfer.body), {}};
  }
  struct Backoff {
    unsigned failures = 0;
    Clock::time_point until{};
  };
  std::string base_;
  std::mutex mutex_;
  std::map<std::string, Endpoint, std::less<>> endpoints_;
  std::map<std::string, Backoff, std::less<>> backoff_;
};
}  // namespace
bool valid_next_page_url(std::string_view url) {
  // Meta Platform 1.205 paging attaches Bearer to absolute URLs. Require the
  // pinned origin first; raw path restrictions prevent normalization escapes.
  constexpr std::string_view origin = "https://graph.oculus.com";
  if (!clean(url) || !url.starts_with(origin) || url.find('#') != url.npos) return false;
  auto tail = url.substr(origin.size());
  if (tail.starts_with(":443")) tail.remove_prefix(4);
  if (!tail.starts_with("/v1.20/")) return false;
  auto path = tail.substr(0, tail.find('?'));
  if (path.find('%') != path.npos || path.find("..") != path.npos || path.find("//") != path.npos)
    return false;
  return true;
}
Error http_response_error(const HttpResponse& r) {
  if (r.error) {
    auto e = failure(r.error.code);
    e.http_status = r.status;
    return e;
  }
  const auto body = Json::parse(r.body, nullptr, false);
  auto code = r.status == 401 || r.status == 403 || graph_190(body) ? ErrorCode::unauthenticated
                                                                    : ErrorCode::unavailable;
  if (r.status >= 200 && r.status < 300 && !graph_190(body)) {
    if (!body.is_object())
      code = ErrorCode::invalid_response;
    else if (!(body.contains("error") && !body["error"].is_null()) &&
             !(body.contains("errors") && !body["errors"].empty()))
      return {};
  }
  auto e = failure(code);
  e.http_status = r.status;
  return e;
}
std::shared_ptr<HttpTransport> meta_http_transport() {
  auto http = std::make_shared<CurlTransport>("https://graph.oculus.com");
  register_identity_iap_endpoints(*http);
  register_social_endpoints(*http);
  register_progression_endpoints(*http);
  return http;
}
std::shared_ptr<HttpTransport> loopback_http_transport(std::uint16_t port) {
  return std::make_shared<CurlTransport>("http://127.0.0.1:" + std::to_string(port));
}
}  // namespace venture::ovrplatform
