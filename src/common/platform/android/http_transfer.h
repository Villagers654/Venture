// SPDX-License-Identifier: GPL-3.0-or-later
// HTTPS on Android through the device's own stack (the app's Http class over
// HttpURLConnection: system TLS, certificate store and proxy). common/http
// drives it with the same header and body callbacks it gives libcurl on
// desktop, so every rule above the transfer (domains, redirects one hop at a
// time, ranges, limits) is the same code.
#pragma once
#include "common/http/client.h"

#include <cstddef>
#include <optional>
#include <stop_token>
#include <string>

namespace venture::http::android {
struct Request {
  std::string url, method = "GET";
  std::optional<std::string> body;
  Fields headers;
  long connect_seconds = 15;
  long read_seconds = 60;  // no byte for this long fails the transfer
};
enum class Failure { none, timeout, tls, network, stopped, write };
struct Result {
  Failure failure = Failure::none;
  std::string message;
  long status = 0;
};
// libcurl's callback shape: returns `size * count` to go on.
using Callback = std::size_t (*)(char*, std::size_t, std::size_t, void*);
// Sends the request and hands the response to the callbacks: the status line
// ("HTTP/1.1 206 ") and each "name: value" header line, then the body.
Result perform(const Request&, std::stop_token, Callback header, void* header_data, Callback write,
               void* write_data);
}  // namespace venture::http::android
