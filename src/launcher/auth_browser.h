#pragma once
#include "common/meta/native_host.h"
#include "common/paths.h"
#include <memory>
#include <optional>
#include <vector>
namespace venture::launcher {
class AuthBrowser {
public:
  enum class Kind { ready, callback, closed, error };
  struct Event {
    Kind kind;
    std::string value;
  };
  AuthBrowser();
  ~AuthBrowser();
  // `cookies`, if any, seed this sign-in's private browsing profile before
  // navigating, so a browser session the user already has with Meta carries
  // over instead of asking them to sign in again. Windows only; a non-empty
  // `cookies` throws elsewhere.
  void open(std::string url, const fs::path &data,
            std::vector<meta::Cookie> cookies = {});
  void close();
  std::optional<Event> poll();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
// POSIX: entry point of the isolated sign-in helper process that AuthBrowser
// starts. Returns -1 when argv is not a helper invocation.
int auth_browser_helper(int argc, char **argv);
} // namespace venture::launcher
