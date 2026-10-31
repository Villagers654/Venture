#include "common/meta/native_host.h"
#include "common/ipc.h"
#include "common/log.h"
#include "common/secret_store.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace venture::meta {
namespace {
constexpr std::string_view kSealContext = "venture.meta_session_cookies.v1";
fs::path cookie_file(const fs::path &data) {
  return data / "meta_session_cookies.dpapi";
}
double real(const Json &j, std::string_view key) {
  auto &v = field(j, key);
  return v.is_number() ? v.get<double>() : 0.0;
}
Json to_json(const Cookie &c) {
  return Json{{"name", c.name},       {"value", c.value},
             {"domain", c.domain},   {"path", c.path},
             {"secure", c.secure},   {"httpOnly", c.http_only},
             {"sameSite", c.same_site},
             {"expirationDate", c.expiration_date}};
}
Cookie from_json(const Json &j) {
  Cookie c;
  c.name = str(j, "name");
  c.value = str(j, "value");
  c.domain = str(j, "domain");
  c.path = str(j, "path");
  c.secure = field(j, "secure") == true;
  c.http_only = field(j, "httpOnly") == true;
  c.same_site = str(j, "sameSite");
  c.expiration_date = real(j, "expirationDate");
  return c;
}
} // namespace

fs::path native_messaging_host_executable() { return paths::executable(); }
Json native_messaging_host_manifest() {
  return Json{
      {"name", std::string(kNativeMessagingHostName)},
      {"description",
       "Delivers your Meta/Oculus browser session to Venture."},
      {"path", paths::utf8(native_messaging_host_executable())},
      {"type", "stdio"},
      {"allowed_origins",
       Json::array({"chrome-extension://" + std::string(kMetaSessionExtensionId) +
                    "/"})}};
}

std::vector<Cookie> parse_cookie_message(const Json &message) {
  if (!message.is_object() || str(message, "type") != "Cookies")
    throw std::runtime_error("Not a Cookies message.");
  const auto &list = field(message, "cookies");
  if (!list.is_array())
    throw std::runtime_error("Cookies message has no cookies array.");
  std::vector<Cookie> cookies;
  cookies.reserve(list.size());
  for (const auto &item : list) {
    Cookie cookie = from_json(item);
    if (cookie.name.empty() || cookie.domain.empty())
      throw std::runtime_error("Cookie is missing its name or domain.");
    cookies.push_back(std::move(cookie));
  }
  return cookies;
}

void write_browser_session_cookies(const fs::path &data,
                                   const std::vector<Cookie> &cookies) {
  Json rows = Json::array();
  for (auto &cookie : cookies)
    rows.push_back(to_json(cookie));
  secret_seal::save(cookie_file(data), kSealContext, rows.dump());
  // Never log a cookie's name, value or domain.
  log::info("meta", "Saved {} browser session cookie(s) from the native messaging host.",
           cookies.size());
}
std::vector<Cookie> read_browser_session_cookies(const fs::path &data) {
  auto plain = secret_seal::load(cookie_file(data), kSealContext);
  if (!plain)
    return {};
  auto rows = parse_json(*plain);
  if (!rows.is_array())
    throw std::runtime_error("Saved browser session cookies are invalid.");
  std::vector<Cookie> cookies;
  cookies.reserve(rows.size());
  for (const auto &item : rows)
    cookies.push_back(from_json(item));
  return cookies;
}
void clear_browser_session_cookies(const fs::path &data) {
  secret_seal::clear(cookie_file(data));
}

int handle_native_messaging_launch(const std::vector<std::string> &args) {
  if (args.size() < 2 || !args[1].starts_with("chrome-extension://"))
    return -1;
  // Belt and suspenders beyond the host manifest's own allowed_origins:
  // reject any other origin without ever touching stdio.
  if (args[1] != "chrome-extension://" + std::string(kMetaSessionExtensionId) + "/") {
    log::error("meta", "Native messaging host: unexpected caller origin.");
    log::flush();
    return 1;
  }
  // stdout carries only the native messaging protocol; diagnostics go to a
  // file instead, the way the launcher's own background processes do.
  log::set_stderr(false);
  try {
    fs::create_directories(paths::data_dir() / "logs");
    log::set_file(paths::utf8(paths::data_dir() / "logs" / "meta-native-host.log"));
  } catch (...) {
  }
  int status = 1;
  try {
#ifdef _WIN32
    auto channel = PipeChannel::inherited(
        reinterpret_cast<std::uintptr_t>(GetStdHandle(STD_INPUT_HANDLE)),
        reinterpret_cast<std::uintptr_t>(GetStdHandle(STD_OUTPUT_HANDLE)));
#else
    auto channel = PipeChannel::inherited(std::uintptr_t(STDIN_FILENO),
                                          std::uintptr_t(STDOUT_FILENO));
#endif
    auto response = channel->accept([](const Json &message) {
      auto cookies = parse_cookie_message(message);
      write_browser_session_cookies(paths::data_dir(), cookies);
      return Json{{"ok", true}};
    });
    status = field(response, "error").is_string() ? 1 : 0;
  } catch (const std::exception &e) {
    log::error("meta", "Native messaging host: {}", e.what());
  } catch (...) {
    log::error("meta", "Native messaging host failed.");
  }
  log::flush();
  return status;
}
} // namespace venture::meta
