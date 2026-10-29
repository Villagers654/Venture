#pragma once
#include "common/paths.h"
#include <functional>
#include <map>
#include <stop_token>
#include <vector>

namespace venture::http {
using Fields = std::vector<std::pair<std::string, std::string>>;
struct Response {
  long status = 0;
  std::string body;
  std::map<std::string, std::string> headers;
};
struct Url {
  std::string scheme, host, port, path, query, user, password;
};
// Points a libcurl handle at the system CA bundle. TLS is linked statically on
// Linux, so its compiled-in paths cannot match every distribution; this picks
// the first existing bundle ($SSL_CERT_FILE first). No-op on Windows.
void use_system_ca(void *curl);
Url parse_url(std::string_view url);
std::string encode(std::string_view text);
std::string decode(std::string_view text);
Fields query(std::string_view text);
std::string form(const Fields &fields);
bool domain(std::string_view host, std::string_view parent);
bool allowed_delivery(std::string_view url);
bool secure_binary(std::string_view url);
std::string with_delivery_token(std::string_view url, std::string_view token);
Response post(std::string_view url, const Fields &fields,
              std::stop_token stop = {}, const Fields &headers = {});
Response get_meta(std::string_view url, const Fields &headers,
                  std::stop_token stop = {});
// Explicit test transport; only the numeric loopback address can receive
// headers.
Response get_loopback(uint16_t port, std::string_view path,
                      const Fields &headers, std::stop_token stop = {});
Response post_loopback(uint16_t port, std::string_view path,
                       const Fields &fields, std::stop_token stop = {});
std::string get_public(std::string url, std::stop_token stop = {});
// Venture's latest published release from GitHub's API; no account data is sent.
Response get_latest_release(std::stop_token stop = {});
std::string get_image(std::string url, std::stop_token stop = {});
// The file name a Meta delivery URL serves (its Content-Disposition), read
// with a one-byte range request; empty when the server names none.
std::string delivery_file_name(std::string url, std::stop_token stop = {});
struct RangeDecision {
  uint64_t offset, total;
};
RangeDecision check_range(long status, uint64_t offset, uint64_t expected,
                          std::string_view content_range, uint64_t length);
using Progress = std::function<void(uint64_t done, uint64_t total)>;
void download(std::string url, const fs::path &destination, uint64_t expected,
              std::stop_token stop, const Progress &progress);
// Venture's own release files: https on github.com or githubusercontent.com only.
bool allowed_release(std::string_view url);
void download_release(std::string url, const fs::path &destination, uint64_t expected,
                      std::stop_token stop, const Progress &progress);
std::string get_release_text(std::string url, std::stop_token stop = {});
} // namespace venture::http
