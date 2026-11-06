#include "common/http/client.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <curl/curl.h>
#include <format>
#include <fstream>
#include <memory>
#include <regex>

namespace venture::http {
namespace {
struct Global {
  Global() {
    if (curl_global_init(CURL_GLOBAL_DEFAULT))
      throw std::runtime_error("HTTP initialization failed.");
  }
  ~Global() { curl_global_cleanup(); }
};
void init() { static Global global; }
// Let's Encrypt's Root YR cross-signed by ISRG Root X1 (letsencrypt.org/certs/
// gen-y/root-yr-by-x1.pem, SHA-256 07:26:39:D0:...:07:CF:76). Hosts such as
// dpaste.com serve chains ending at Root YR, which distribution CA bundles
// predate. Alongside the system roots it only links such a chain to the
// system's own X1: it is not self-signed, so it is never a trust anchor itself.
constexpr char root_yr_by_x1[] =
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "-----END CERTIFICATE-----\n";
#ifdef _WIN32
// ISRG Root X1 (letsencrypt.org/certs/isrgrootx1.pem). Windows verifies
// against its own store, fetching roots it lacks and checking revocation over
// plain HTTP; some networks block that. A verification failure is retried once
// with only Let's Encrypt's roots (X1 and Root YR through X1), so hosts such as
// dpaste.com whose chain ends at the newer Root YR still verify there; any
// other issuer still fails.
constexpr char isrg_root_x1[] =
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "-----END CERTIFICATE-----\n";
#endif
#ifndef _WIN32
const std::string &system_ca_bundle() {
  static const std::string bundle = [] {
    std::vector<std::string> candidates;
    if (auto *env = std::getenv("SSL_CERT_FILE"); env && *env)
      candidates.emplace_back(env);
    for (auto *path : {"/etc/ssl/certs/ca-certificates.crt", // Debian, Ubuntu, Arch, Flatpak
                       "/etc/pki/tls/certs/ca-bundle.crt",   // Fedora, RHEL
                       "/etc/ssl/ca-bundle.pem",             // openSUSE
                       "/etc/pki/tls/cacert.pem", "/etc/ssl/cert.pem"})
      candidates.emplace_back(path);
    std::error_code ec;
    for (auto &path : candidates)
      if (fs::is_regular_file(path, ec))
        return path;
    return std::string();
  }();
  return bundle;
}
const std::string &system_ca_blob() {
  static const std::string blob = [] {
    std::ifstream in(system_ca_bundle(), std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), {});
    if (!text.empty() && text.back() != '\n') text += '\n';
    return text + root_yr_by_x1;
  }();
  return blob;
}
#endif
using Handle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
using UrlHandle = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>;
} // namespace
void use_system_ca(void *curl) {
#ifndef _WIN32
  if (!system_ca_bundle().empty()) {
    auto &blob = system_ca_blob();
    curl_blob data{const_cast<char *>(blob.data()), blob.size(), CURL_BLOB_NOCOPY};
    curl_easy_setopt(static_cast<CURL *>(curl), CURLOPT_CAINFO_BLOB, &data);
  }
#else
  // Schannel fails the handshake when the certificate's revocation servers
  // cannot be reached (a blocked or filtered network); a certificate that is
  // known revoked is still refused.
  curl_easy_setopt(static_cast<CURL *>(curl), CURLOPT_SSL_OPTIONS, long(CURLSSLOPT_REVOKE_BEST_EFFORT));
#endif
}
namespace {
Handle handle(std::string_view url, std::stop_token *stop) {
  init();
  Handle h(curl_easy_init(), curl_easy_cleanup);
  if (!h)
    throw std::runtime_error("HTTP initialization failed.");
  use_system_ca(h.get());
  curl_easy_setopt(h.get(), CURLOPT_URL, std::string(url).c_str());
  curl_easy_setopt(h.get(), CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(h.get(), CURLOPT_REDIR_PROTOCOLS_STR, "https");
  curl_easy_setopt(h.get(), CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(h.get(), CURLOPT_LOW_SPEED_LIMIT, 1L);
  curl_easy_setopt(h.get(), CURLOPT_LOW_SPEED_TIME, 60L);
  curl_easy_setopt(h.get(), CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(h.get(), CURLOPT_USERAGENT, "Venture/0.3 native");
  curl_easy_setopt(h.get(), CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(
      h.get(), CURLOPT_XFERINFOFUNCTION,
      +[](void *p, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
        return static_cast<std::stop_token *>(p)->stop_requested() ? 1 : 0;
      });
  curl_easy_setopt(h.get(), CURLOPT_XFERINFODATA, stop);
  return h;
}
std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}
std::string trim(std::string_view s) {
  auto first = s.find_first_not_of(" \r\n\t");
  if (first == s.npos)
    return {};
  return std::string(
      s.substr(first, s.find_last_not_of(" \r\n\t") - first + 1));
}
size_t headers(char *data, size_t size, size_t count, void *ctx) {
  auto n = size * count;
  auto &r = *static_cast<Response *>(ctx);
  std::string_view line(data, n);
  if (line.starts_with("HTTP/")) {
    r.headers.clear();
    auto pos = line.find(' ');
    r.status = pos != line.npos
                   ? long(number(Json(std::string(line.substr(pos + 1, 3)))))
                   : 0;
  } else if (auto p = line.find(':'); p != line.npos) {
    r.headers[lower(std::string(line.substr(0, p)))] = trim(line.substr(p + 1));
  }
  return n;
}
void check(CURLcode code, std::stop_token stop) {
  if (stop.stop_requested())
    throw std::runtime_error("Transfer paused. Resume when ready.");
  if (code == CURLE_OPERATION_TIMEDOUT)
    throw std::runtime_error(
        "Connection timed out. Check your connection and retry.");
  // A connection that works but cannot be verified is not the user's network.
  if (code == CURLE_PEER_FAILED_VERIFICATION || code == CURLE_SSL_CACERT_BADFILE)
    throw std::runtime_error(std::format(
        "The secure connection could not be verified ({}). This system's certificate store may be out of date.",
        curl_easy_strerror(code)));
  if (code != CURLE_OK)
    throw std::runtime_error(std::format(
        "Network transfer failed ({}). Check your connection and retry.", curl_easy_strerror(code)));
}
std::string redirect(std::string_view from, const Response &r,
                     bool (*allowed)(std::string_view) = allowed_delivery) {
  auto i = r.headers.find("location");
  if (i == r.headers.end())
    throw std::runtime_error("File redirect is missing its destination.");
  UrlHandle u(curl_url(), curl_url_cleanup);
  if (curl_url_set(u.get(), CURLUPART_URL, std::string(from).c_str(), 0) ||
      curl_url_set(u.get(), CURLUPART_URL, i->second.c_str(), 0))
    throw std::runtime_error("Invalid file redirect.");
  char *text = nullptr;
  curl_url_get(u.get(), CURLUPART_URL, &text, 0);
  std::string result = text ? text : "";
  curl_free(text);
  if (!allowed(result))
    throw std::runtime_error(allowed == allowed_delivery ? "File redirect is outside Meta delivery domains."
                                                         : "Update redirect is outside Venture's GitHub releases.");
  // A credential in a Location must obey the same host restriction as the
  // initial request.
  for (auto &[key, value] : query(parse_url(result).query))
    if (key == "access_token" && !secure_binary(result))
      throw std::runtime_error("Refused a credential-bearing file redirect.");
  return result;
}
bool redirected(long status) {
  return status == 301 || status == 302 || status == 303 || status == 307 ||
         status == 308;
}
Response memory(std::string_view url, const Fields *fields, size_t limit,
                std::stop_token stop, const Fields &extra_headers = {},
                bool loopback = false) {
  auto h = handle(url, &stop);
  if (loopback) {
    curl_easy_setopt(h.get(), CURLOPT_PROTOCOLS_STR, "http");
    curl_easy_setopt(h.get(), CURLOPT_NOPROXY, "*");
  }
  Response r;
  struct Sink {
    std::string *text;
    size_t limit;
  } sink{&r.body, limit};
  curl_easy_setopt(h.get(), CURLOPT_TIMEOUT, 40L);
  if (!extra_headers.empty() && !fields) {
    curl_easy_setopt(h.get(), CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(h.get(), CURLOPT_TIMEOUT, 15L);
  }
  curl_easy_setopt(h.get(), CURLOPT_HEADERFUNCTION, headers);
  curl_easy_setopt(h.get(), CURLOPT_HEADERDATA, &r);
  curl_easy_setopt(
      h.get(), CURLOPT_WRITEFUNCTION,
      +[](char *b, size_t s, size_t c, void *p) -> size_t {
        auto &out = *static_cast<Sink *>(p);
        auto n = s * c;
        if (out.text->size() + n > out.limit)
          return 0;
        try {
          out.text->append(b, n);
        } catch (...) {
          return 0;
        }
        return n;
      });
  curl_easy_setopt(h.get(), CURLOPT_WRITEDATA, &sink);
  std::string body;
  curl_slist *raw_headers = nullptr;
  if (fields) {
    body = form(*fields);
    curl_easy_setopt(h.get(), CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(h.get(), CURLOPT_POSTFIELDSIZE_LARGE,
                     curl_off_t(body.size()));
  }
  raw_headers = curl_slist_append(nullptr, "Accept: application/json");
  if (!raw_headers)
    throw std::runtime_error("HTTP initialization failed.");
  std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> hs(
      nullptr, curl_slist_free_all);
  hs.reset(raw_headers);
  for (auto &[key, value] : extra_headers) {
    if (key.empty() || key.find_first_of("\r\n:") != key.npos ||
        value.find_first_of("\r\n") != value.npos)
      throw std::runtime_error("Invalid HTTP header.");
    auto *next = curl_slist_append(raw_headers, (key + ": " + value).c_str());
    if (!next)
      throw std::runtime_error("HTTP initialization failed.");
    hs.release();
    hs.reset(raw_headers = next);
  }
  curl_easy_setopt(h.get(), CURLOPT_HTTPHEADER, raw_headers);
  auto code = curl_easy_perform(h.get());
#ifdef _WIN32
  if (code == CURLE_PEER_FAILED_VERIFICATION && !loopback) {
    static const std::string roots = std::string(isrg_root_x1) + root_yr_by_x1;
    curl_blob data{const_cast<char *>(roots.data()), roots.size(), CURL_BLOB_NOCOPY};
    curl_easy_setopt(h.get(), CURLOPT_CAINFO_BLOB, &data);
    r.body.clear();
    r.headers.clear();
    code = curl_easy_perform(h.get());
  }
#endif
  curl_easy_getinfo(h.get(), CURLINFO_RESPONSE_CODE, &r.status);
  check(code, stop);
  return r;
}
} // namespace
Url parse_url(std::string_view url) {
  init();
  UrlHandle u(curl_url(), curl_url_cleanup);
  if (curl_url_set(u.get(), CURLUPART_URL, std::string(url).c_str(),
                   CURLU_NON_SUPPORT_SCHEME))
    throw std::runtime_error("Invalid URL.");
  auto part = [&](CURLUPart key) {
    char *p = nullptr;
    curl_url_get(u.get(), key, &p, 0);
    std::string s = p ? p : "";
    curl_free(p);
    return s;
  };
  return {lower(part(CURLUPART_SCHEME)), lower(part(CURLUPART_HOST)),
          part(CURLUPART_PORT),          part(CURLUPART_PATH),
          part(CURLUPART_QUERY),         part(CURLUPART_USER),
          part(CURLUPART_PASSWORD)};
}
std::string encode(std::string_view text) {
  init();
  char *p = curl_easy_escape(nullptr, text.data(), int(text.size()));
  if (!p)
    throw std::runtime_error("URL encoding failed.");
  std::string out(p);
  curl_free(p);
  return out;
}
std::string decode(std::string_view text) {
  init();
  auto s = std::string(text);
  std::replace(s.begin(), s.end(), '+', ' ');
  int n = 0;
  char *p = curl_easy_unescape(nullptr, s.data(), int(s.size()), &n);
  if (!p)
    throw std::runtime_error("URL decoding failed.");
  std::string out(p, n);
  curl_free(p);
  return out;
}
Fields query(std::string_view text) {
  Fields fields;
  while (!text.empty()) {
    auto end = text.find('&');
    auto pair = text.substr(0, end);
    auto eq = pair.find('=');
    fields.emplace_back(decode(pair.substr(0, eq)),
                        eq == pair.npos ? "" : decode(pair.substr(eq + 1)));
    if (end == text.npos)
      break;
    text.remove_prefix(end + 1);
  }
  return fields;
}
std::string form(const Fields &fields) {
  std::string body;
  for (auto &[key, value] : fields) {
    if (!body.empty())
      body += '&';
    body += encode(key) + '=' + encode(value);
  }
  return body;
}
bool domain(std::string_view host, std::string_view parent) {
  return host == parent ||
         (host.size() > parent.size() &&
          host.ends_with(std::string(".") + std::string(parent)));
}
bool allowed_release(std::string_view url) {
  try {
    auto u = parse_url(url);
    if (u.scheme != "https" || !u.user.empty() || !u.password.empty() ||
        (!u.port.empty() && u.port != "443"))
      return false;
    return u.host == "github.com" || domain(u.host, "githubusercontent.com");
  } catch (...) {
  }
  return false;
}
bool allowed_delivery(std::string_view url) {
  try {
    auto u = parse_url(url);
    if (u.scheme != "https" || !u.user.empty() || !u.password.empty() ||
        (!u.port.empty() && u.port != "443"))
      return false;
    for (auto host : {"oculus.com", "oculuscdn.com", "facebook.com",
                      "fbcdn.net", "meta.com", "fbsbx.com"})
      if (domain(u.host, host))
        return true;
  } catch (...) {
  }
  return false;
}
bool secure_binary(std::string_view url) {
  try {
    auto u = parse_url(url);
    static const std::regex host(R"(^securecdn(-[a-z0-9-]+)?\.oculus\.com$)");
    return allowed_delivery(url) && u.path == "/binaries/download/" &&
           std::regex_match(u.host, host);
  } catch (...) {
    return false;
  }
}
std::string with_delivery_token(std::string_view url, std::string_view token) {
  if (!allowed_delivery(url))
    throw std::runtime_error("File URL is outside Meta delivery domains.");
  if (!secure_binary(url))
    return std::string(url);
  auto pairs = query(parse_url(url).query);
  std::erase_if(pairs, [](auto &p) { return p.first == "access_token"; });
  pairs.emplace_back("access_token", token);
  auto base = std::string(url.substr(0, url.find_first_of("?#")));
  return base + '?' + form(pairs);
}
Response post(std::string_view url, const Fields &fields, std::stop_token stop,
              const Fields &headers) {
  return memory(url, &fields, 16 * 1024 * 1024, stop, headers);
}
Response get_meta(std::string_view url, const Fields &headers,
                  std::stop_token stop) {
  auto u = parse_url(url);
  if (u.scheme != "https" || u.host != "graph.oculus.com" ||
      (!u.port.empty() && u.port != "443") || !u.user.empty() ||
      !u.password.empty() ||
      (u.path != "/v1.20/application/available_purchases" &&
       u.path != "/v1.20/application/viewer_purchases"))
    throw std::runtime_error("Refused an invalid Meta commerce URL.");
  return memory(url, nullptr, 4 * 1024 * 1024, stop, headers);
}
Response get_latest_release(std::stop_token stop) {
  return memory("https://api.github.com/repos/Villagers654/Venture/releases/latest", nullptr, 1024 * 1024, stop,
                {{"Accept", "application/vnd.github+json"}});
}
Response get_oculusdb_package(std::string_view package, std::stop_token stop) {
  if (!paths::safe_package(package))
    throw std::runtime_error("Invalid package name.");
  return memory("https://oculusdb.rui2015.me/api/v1/packagename/" + encode(package), nullptr, 1024 * 1024, stop,
                {{"Accept", "application/json"}});
}
Response get_loopback(uint16_t port, std::string_view path,
                      const Fields &headers, std::stop_token stop) {
  if (!port || !path.starts_with('/') || path.starts_with("//") ||
      path.find_first_of("\r\n#") != path.npos)
    throw std::runtime_error("Invalid loopback test URL.");
  return memory("http://127.0.0.1:" + std::to_string(port) + std::string(path),
                nullptr, 4 * 1024 * 1024, stop, headers, true);
}
std::string get_public(std::string url, std::stop_token stop) {
  return get_image(std::move(url), stop);
}
Response post_loopback(uint16_t port, std::string_view path, const Fields &fields,
                       std::stop_token stop) {
  if (!port || !path.starts_with('/') || path.starts_with("//") ||
      path.find_first_of("\r\n#") != path.npos)
    throw std::runtime_error("Invalid loopback test URL.");
  return memory("http://127.0.0.1:" + std::to_string(port) + std::string(path),
                &fields, 4 * 1024 * 1024, stop, {}, true);
}
std::string get_image(std::string url, std::stop_token stop) {
  for (int i = 0; i < 8; ++i) {
    if (!allowed_delivery(url))
      throw std::runtime_error("Artwork URL is outside Meta delivery domains.");
    auto r = memory(url, nullptr, 20 * 1024 * 1024, stop);
    if (redirected(r.status)) {
      url = redirect(url, r);
      continue;
    }
    if (r.status != 200)
      throw std::runtime_error("Artwork is unavailable. Refresh to retry.");
    return r.body;
  }
  throw std::runtime_error("Too many artwork redirects.");
}
static int hex_digit(char c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
std::string delivery_file_name(std::string url, std::stop_token stop) {
  for (int i = 0; i < 8; ++i) {
    if (!allowed_delivery(url))
      throw std::runtime_error("File URL is outside Meta delivery domains.");
    auto r = memory(url, nullptr, 64 * 1024, stop, {{"Range", "bytes=0-0"}});
    if (redirected(r.status)) {
      url = redirect(url, r);
      continue;
    }
    if (r.status != 200 && r.status != 206)
      throw std::runtime_error("File server refused the request (HTTP " + std::to_string(r.status) + ").");
    // RFC 6266: filename* (RFC 5987, UTF-8) is the exact name when present.
    auto disposition = r.headers["content-disposition"];
    std::string name;
    if (auto p = disposition.find("filename*="); p != disposition.npos) {
      auto value = trim(disposition.substr(p + 10, disposition.find(';', p) - p - 10));
      if (auto quote = value.find("''"); quote != value.npos &&
          lower(value.substr(0, quote)) == "utf-8")
        for (std::size_t i = quote + 2; i < value.size(); ++i) {
          // Percent escapes only: '+' is a literal in RFC 5987 values.
          int hi, lo;
          if (value[i] == '%' && i + 2 < value.size() &&
              (hi = hex_digit(value[i + 1])) >= 0 && (lo = hex_digit(value[i + 2])) >= 0) {
            name += char(hi * 16 + lo);
            i += 2;
          } else {
            name += value[i];
          }
        }
    }
    if (auto p = disposition.find("filename="); name.empty() && p != disposition.npos) {
      name = trim(disposition.substr(p + 9, disposition.find(';', p) - p - 9));
      if (name.size() >= 2 && name.front() == '"' && name.back() == '"')
        name = name.substr(1, name.size() - 2);
    }
    return name;
  }
  throw std::runtime_error("Too many file redirects.");
}
RangeDecision check_range(long status, uint64_t offset, uint64_t expected,
                          std::string_view range, uint64_t length) {
  if (status != 200 && status != 206)
    throw std::runtime_error("File server refused the transfer (HTTP " +
                             std::to_string(status) +
                             "). Sign in again, then resume.");
  if (status == 200)
    return {0, expected ? expected : length};
  static const std::regex pattern(R"(^bytes ([0-9]+)-([0-9]+)/([0-9]+)$)");
  std::smatch m;
  std::string s(range);
  if (!std::regex_match(s, m, pattern))
    throw std::runtime_error("Invalid resume range. Retry the transfer.");
  auto start = number(Json(m[1].str())), end = number(Json(m[2].str())),
       total = number(Json(m[3].str()));
  if (start != offset || end < start || total <= end ||
      (expected && total != expected) || (length && length != end - start + 1))
    throw std::runtime_error("Inconsistent resume range. Retry the transfer.");
  return {offset, expected ? expected : total};
}
namespace {
void fetch_file(std::string url, const fs::path &dest, uint64_t expected, std::stop_token stop,
                const Progress &progress, bool (*allowed)(std::string_view), const char *outside);
}
void download(std::string url, const fs::path &dest, uint64_t expected,
              std::stop_token stop, const Progress &progress) {
  fetch_file(std::move(url), dest, expected, stop, progress, allowed_delivery,
             "File URL is outside Meta delivery domains.");
}
void download_release(std::string url, const fs::path &dest, uint64_t expected,
                      std::stop_token stop, const Progress &progress) {
  fetch_file(std::move(url), dest, expected, stop, progress, allowed_release,
             "Update URL is outside Venture's GitHub releases.");
}
std::string get_release_text(std::string url, std::stop_token stop) {
  for (int i = 0; i < 8; ++i) {
    if (!allowed_release(url))
      throw std::runtime_error("Update URL is outside Venture's GitHub releases.");
    auto r = memory(url, nullptr, 1024 * 1024, stop);
    if (redirected(r.status)) {
      url = redirect(url, r, allowed_release);
      continue;
    }
    if (r.status != 200)
      throw std::runtime_error("GitHub returned HTTP " + std::to_string(r.status) + ".");
    return r.body;
  }
  throw std::runtime_error("Too many redirects. Try again later.");
}
namespace {
void fetch_file(std::string url, const fs::path &dest, uint64_t expected, std::stop_token stop,
                const Progress &progress, bool (*allowed)(std::string_view), const char *outside) {
  fs::create_directories(dest.parent_path());
  auto part = dest;
  part += ".part";
  if (expected && fs::is_regular_file(dest) &&
      fs::file_size(dest) == expected) {
    progress(expected, expected);
    return;
  }
  uint64_t offset = fs::exists(part) ? fs::file_size(part) : 0;
  if (expected && offset > expected) {
    fs::resize_file(part, 0);
    offset = 0;
  }
  // A complete .part from a crash is already size checked before publishing.
  if (expected && offset == expected) {
    paths::replace_file(part, dest);
    progress(expected, expected);
    return;
  }
  for (int redirects = 0; redirects < 8; ++redirects) {
    if (!allowed(url))
      throw std::runtime_error(outside);
    if (stop.stop_requested())
      throw std::runtime_error("Transfer paused. Resume when ready.");
    Response r;
    auto h = handle(url, &stop);
    if (offset)
      curl_easy_setopt(h.get(), CURLOPT_RANGE,
                       (std::to_string(offset) + '-').c_str());
    curl_easy_setopt(h.get(), CURLOPT_HEADERFUNCTION, headers);
    curl_easy_setopt(h.get(), CURLOPT_HEADERDATA, &r);
    struct Sink {
      Response *response;
      fs::path path;
      std::ofstream out;
      uint64_t offset, expected, done = 0, total = 0;
      bool opened = false;
      std::string error;
      const Progress *progress;
    } sink{&r, part, {}, offset, expected, 0, 0, false, {}, &progress};
    curl_easy_setopt(
        h.get(), CURLOPT_WRITEFUNCTION,
        +[](char *b, size_t s, size_t c, void *p) -> size_t {
          auto &out = *static_cast<Sink *>(p);
          auto n = s * c;
          if (redirected(out.response->status))
            return n;
          try {
            if (!out.opened) {
              auto &hdr = out.response->headers;
              auto type = lower(hdr["content-type"]);
              if (type.find("text/html") != type.npos ||
                  type.find("application/json") != type.npos)
                throw std::runtime_error("File server returned an error page. "
                                         "Sign in again, then resume.");
              auto d = check_range(out.response->status, out.offset,
                                   out.expected, hdr["content-range"],
                                   number(Json(hdr["content-length"])));
              out.done = d.offset;
              out.total = d.total;
              out.out.open(out.path,
                           std::ios::binary |
                               (d.offset ? std::ios::app : std::ios::trunc));
              out.opened = true;
            }
            if (out.total && n > out.total - out.done)
              throw std::runtime_error(
                  "File exceeded its expected size. Retry the transfer.");
            out.out.write(b, n);
            if (!out.out)
              throw std::runtime_error("Could not write download. Check free "
                                       "disk space and folder permissions.");
            out.done += n;
            (*out.progress)(out.done, out.total);
            return n;
          } catch (const std::exception &e) {
            out.error = e.what();
            return 0;
          }
        });
    curl_easy_setopt(h.get(), CURLOPT_WRITEDATA, &sink);
    auto code = curl_easy_perform(h.get());
    sink.out.close();
    curl_easy_getinfo(h.get(), CURLINFO_RESPONSE_CODE, &r.status);
    if (!sink.error.empty())
      throw std::runtime_error(sink.error);
    check(code, stop);
    if (redirected(r.status)) {
      url = redirect(url, r, allowed);
      continue;
    }
    if (!sink.opened || !sink.done || (sink.total && sink.done != sink.total) ||
        sink.out.fail())
      throw std::runtime_error(
          "File is incomplete. Resume to retry the remaining bytes.");
    paths::replace_file(part, dest);
    return;
  }
  throw std::runtime_error("Too many file redirects. Try again later.");
}
} // namespace
} // namespace venture::http
