#pragma once
#include "common/json.h"
#include "common/paths.h"
#include <vector>

namespace venture::meta {
// Name published in the native messaging host manifest and in every
// browser's registration of it; the sideloaded extension under
// tools/meta-session-extension/ must ask for this exact host name.
inline constexpr std::string_view kNativeMessagingHostName =
    "com.venture.meta_session";
// Pinned by tools/meta-session-extension/wxt.config.ts's manifest `key`,
// which fixes this id across reloads. The manifest's allowed_origins accepts
// only this origin, so the host answers no one else. If that key is ever
// rotated, this must change to match (see the TODO beside it).
inline constexpr std::string_view kMetaSessionExtensionId =
    "cdaemjfgmclhmheafddnmmgdgjlkjhnh";

// One Meta/Oculus browser cookie, as the sideloaded extension reads it with
// chrome.cookies.getAll() and hands it to the native messaging host.
struct Cookie {
  std::string name, value, domain, path;
  bool secure = false, http_only = false;
  std::string same_site; // "no_restriction" | "lax" | "strict" | "unspecified"
  double expiration_date = 0; // seconds since epoch; 0 for a session cookie
};

// The native messaging host executable: this launcher's own
// paths::executable(). Chrome always starts a native messaging host as a
// fresh process, so this process re-enters itself as the host rather than
// shipping a second executable; see handle_native_messaging_launch() below.
fs::path native_messaging_host_executable();
// The manifest naming native_messaging_host_executable(), as published to
// every browser's NativeMessagingHosts location and, on Windows, as the
// file its registry keys point to.
Json native_messaging_host_manifest();

// Publishes the manifest and, on Windows, the matching HKCU keys for
// Chrome, Edge and Brave, pointed at native_messaging_host_executable().
// Safe to call on every launcher start: always rewritten to the current
// executable's path. Throws std::runtime_error on failure.
void register_native_messaging_host();

// Validates one native-messaging request and extracts its cookies. Throws
// std::runtime_error when `message` is not a well-formed
// {type: "Cookies", cookies: [...]} message.
std::vector<Cookie> parse_cookie_message(const Json &message);

// Seals `cookies` to <data>/meta_session_cookies.dpapi: Windows DPAPI, or on
// POSIX the same sealed-file fallback SecretStore uses with no reachable
// keyring (see secret_seal in common/secret_store.h). Called by the native
// messaging host process after it reads them from the extension.
void write_browser_session_cookies(const fs::path &data,
                                   const std::vector<Cookie> &cookies);
// Unseals the cookies the native messaging host last wrote. Empty when none
// have been delivered yet.
std::vector<Cookie> read_browser_session_cookies(const fs::path &data);
// Removes a previously written cookie file, if any.
void clear_browser_session_cookies(const fs::path &data);

// Chrome's native messaging protocol gives the host no argv of our
// choosing, only the calling extension's origin, as args[1]
// ("chrome-extension://<id>/"). That shape never occurs on an ordinary
// launch, so it doubles as this run's signal that the current process IS
// the native messaging host, the same way launcher::auth_browser_helper()
// uses its own argv shape. -1 when `args` is not that shape; otherwise reads
// exactly one Cookies message from this process's own stdio, seals it, and
// returns the process's exit code.
int handle_native_messaging_launch(const std::vector<std::string> &args);
} // namespace venture::meta
