#pragma once
#include "common/paths.h"
#include "common/ipc.h"
#include <memory>
#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <vector>
namespace venture {
std::uint32_t current_process_id();
// Implements CommandLineToArgvW-compatible escaping; also tested off Windows.
std::wstring quote_windows_arg(std::wstring_view argument);
// Variables set (or replaced) in a child's environment on top of the
// launcher's own.
using Environment = std::vector<std::pair<std::string, std::string>>;
class Process {
public:
  Process();
  ~Process();
  Process(Process &&) noexcept;
  Process &operator=(Process &&) noexcept;
  void spawn(const fs::path &executable, const std::vector<std::string> &args,
             const fs::path &log, bool console);
  // `log` receives the child's stdout and stderr.
  void spawn(const fs::path &executable, const std::vector<std::string> &args,
             const fs::path &log, bool console, PipeChannel::Handler broker,
             const Environment &environment = {});
  bool active() const;
  std::optional<int> poll();
  // Asks a running venture-runtime to quit normally (the Android Activity
  // lifecycle): its stop event on Windows, SIGTERM on POSIX. False when the
  // request could not be delivered.
  bool request_stop();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
// Runs a command-line tool (adb, for one) without a console window: stdin
// empty, stdout handed to `output` as it arrives, stderr kept (up to 64 KiB)
// in `error`. A stop request terminates the tool. Returns its exit code;
// throws when it cannot be started.
int run_tool(const fs::path &executable, const std::vector<std::string> &args,
             const std::function<void(std::string_view)> &output,
             std::string *error = nullptr, std::stop_token stop = {});
void open_folder(const fs::path &directory);
// Shows a file selected in the file manager (just its folder where the file
// manager cannot select one).
void reveal_file(const fs::path &file);
// Opens a web link with the desktop's handler; throws if none accepted it.
void open_url(const std::string &url);
std::string tail_log(const fs::path &file, size_t limit = 16 * 1024);
} // namespace venture
