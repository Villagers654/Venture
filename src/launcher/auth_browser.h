#pragma once
#include "common/paths.h"
#include <memory>
#include <optional>
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
  void open(std::string url, const fs::path &data);
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
