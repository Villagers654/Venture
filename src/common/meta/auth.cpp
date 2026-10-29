#include "common/meta/auth.h"
#include "common/digest.h"
#include <optional>
#include <regex>

namespace venture::meta {
std::string sha256(std::string_view text) {
  return digest::sha256({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}
bool is_callback(std::string_view u) {
  auto scheme = u.substr(0, u.find(':'));
  std::string s(scheme);
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return s == "oculus" || s == "oculus-client";
}
namespace {
std::optional<http::Url> plain_https(std::string_view url) {
  try {
    auto u = http::parse_url(url);
    if (u.scheme == "https" && u.user.empty() && u.password.empty() &&
        (u.port.empty() || u.port == "443"))
      return u;
  } catch (...) {
  }
  return {};
}
} // namespace
bool auth_navigation_allowed(std::string_view url) {
  if (auto u = plain_https(url))
    for (auto host : {"meta.com", "facebook.com", "fbsbx.com", "oculus.com",
                      "instagram.com"})
      if (http::domain(u->host, host))
        return true;
  return false;
}
bool auth_frame_allowed(std::string_view url) {
  return url == "about:blank" || url == "about:srcdoc" || plain_https(url).has_value();
}
std::string validate_callback(std::string_view url,
                              std::string_view challenge) {
  if (!is_callback(url) || challenge.empty() || url.size() > 3 * 1024 * 1024 ||
      url.find('?') == url.npos)
    throw std::runtime_error("Invalid sign-in callback. Start sign-in again.");
  auto q = url.substr(url.find('?') + 1);
  q = q.substr(0, q.find('#'));
  std::string token, blob;
  unsigned tokens = 0, blobs = 0;
  for (auto &[key, value] : http::query(q)) {
    if (key == "token") {
      token = value;
      ++tokens;
    }
    if (key == "blob") {
      blob = value;
      ++blobs;
    }
  }
  const auto expected = sha256(challenge).substr(0, 16);
  unsigned mismatch = token.size() != expected.size();
  // Fixed iteration count, no early exit on a matching prefix.
  volatile unsigned difference = 0;
  for (size_t i = 0; i < expected.size(); ++i)
    difference =
        difference |
        (static_cast<unsigned char>(expected[i]) ^
         (i < token.size() ? static_cast<unsigned char>(token[i]) : 0));
  mismatch |= difference;
  if (mismatch || tokens != 1 || blobs != 1 || blob.empty() ||
      blob.size() > 2 * 1024 * 1024)
    throw std::runtime_error(
        "This sign-in callback belongs to a different session. Start sign-in again.");
  return blob;
}
Json post(std::string_view url, const http::Fields &fields,
          std::stop_token stop, const http::Fields &headers) {
  auto r = http::post(url, fields, stop, headers);
  auto j = Json::parse(r.body, nullptr, false);
  auto error = field(j, "error");
  auto &errors = field(j, "errors");
  if (error.is_null() && errors.is_array() && !errors.empty())
    error = errors[0];
  if (r.status < 200 || r.status >= 300 || !error.is_null()) {
    auto endpoint = http::parse_url(url).path;
    endpoint = endpoint.substr(endpoint.rfind('/') + 1);
    std::string detail;
    auto type = str(error, "type");
    if (std::regex_match(type, std::regex("[A-Za-z_]{1,64}")))
      detail += " " + type;
    auto code = str(error, "code");
    if (digits(code, 1, 10))
      detail += " code " + code;
    auto sub = str(error, "error_subcode");
    if (digits(sub, 1, 10))
      detail += " subcode " + sub;
    throw RequestError(r.status, "Meta " + endpoint + " failed (HTTP " +
                             std::to_string(r.status) + detail +
                             "). Try signing in again.");
  }
  if (j.is_discarded())
    throw std::runtime_error(
        "Meta's reply could not be read. Try again later.");
  return j;
}
std::string Auth::begin(std::stop_token stop) {
  auto j = post("https://meta.graph.meta.com/webview_tokens_query",
                {{"access_token",
                  "FRL|512466987071624|01d4a1f7fd0682aea7ee8ae987704d63"}},
                stop);
  challenge_ = str(j, "native_sso_token");
  auto etoken = str(j, "native_sso_etoken");
  if (challenge_.empty() || etoken.empty())
    throw std::runtime_error("Meta's reply had no sign-in challenge.");
  expires_ = std::chrono::steady_clock::now() + std::chrono::minutes(10);
  return "https://auth.meta.com/native_sso/confirm?" +
         http::form({{"native_app_id", "512466987071624"},
                     {"source_app_id", "512466987071624"},
                     {"native_sso_etoken", etoken},
                     {"utm_source", "venture"}});
}
Session Auth::complete(std::string_view callback, std::stop_token stop) {
  if (std::chrono::steady_clock::now() > expires_)
    throw std::runtime_error("Sign-in expired. Start sign-in again.");
  auto blob = validate_callback(callback, challenge_);
  auto challenge = std::exchange(challenge_, {});
  auto j = post(
      "https://meta.graph.meta.com/webview_blobs_decrypt",
      {{"access_token", "FRL|512466987071624|01d4a1f7fd0682aea7ee8ae987704d63"},
       {"blob", blob},
       {"request_token", challenge}},
      stop);
  auto token = str(j, "access_token");
  if (token.empty())
    throw std::runtime_error("Meta did not return a sign-in token.");
  j = post("https://meta.graph.meta.com/graphql",
           {{"access_token", token},
            {"doc_id", "24112177345042346"},
            {"variables", Json{{"app_id", "1582076955407037"}}.dump()}},
           stop);
  auto &profiles = field(field(field(j, "data"), "xfr_create_profile_token"),
                         "profile_tokens");
  if (!profiles.is_array() || profiles.empty() ||
      str(profiles[0], "access_token").empty())
    throw std::runtime_error("Meta's reply had no Quest account token.");
  return Session{str(profiles[0], "access_token"),
                 {},
                 {},
                 paths::timestamp(),
                 "native-sso",
                 str(profiles[0], "profile_id")};
}
} // namespace venture::meta
