#include "common/http/client.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <curl/curl.h>
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
#endif
using Handle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
using UrlHandle = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>;
} // namespace
void use_system_ca(void *curl) {
#ifndef _WIN32
  if (auto &bundle = system_ca_bundle(); !bundle.empty())
    curl_easy_setopt(static_cast<CURL *>(curl), CURLOPT_CAINFO, bundle.c_str());
#else
  (void)curl;
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
  if (code != CURLE_OK)
    throw std::runtime_error(
        "Network transfer failed. Check your connection and retry.");
}
std::string redirect(std::string_view from, const Response &r) {
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
  if (!allowed_delivery(result))
    throw std::runtime_error("File redirect is outside Meta delivery domains.");
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
void download(std::string url, const fs::path &dest, uint64_t expected,
              std::stop_token stop, const Progress &progress) {
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
    if (!allowed_delivery(url))
      throw std::runtime_error("File URL is outside Meta delivery domains.");
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
      url = redirect(url, r);
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
} // namespace venture::http
