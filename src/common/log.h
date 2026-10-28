// Minimal thread-safe logging shared by the launcher and the runtime.
#pragma once

#include <chrono>
#include <format>
#include <string_view>

namespace venture::log {

enum class Level { trace, debug, info, warn, error };

void set_level(Level level);
Level level();
// Explicitly enable writing to the attached Windows console, even when stdio
// is redirected. The runtime calls this only when passed --console.
// No-op when there is no console, and on POSIX.
void attach_console();
// Also append every message to this file (UTF-8). Empty path disables it.
// A warning or error repeated verbatim (same level, tag and text) is written
// once; later copies are counted and written as "<message> (N times in the
// last S s)", standing for N occurrences, at most once per repeat period (10 s),
// within two periods after a burst stops, and for any pending count when
// the file is closed or replaced and at exit.
// Each new file starts with fresh first occurrences.
void set_file(std::string_view path);
// With a file set, stop mirroring warnings and errors to stderr (a command
// line tool whose terminal output is its own; default on).
void set_stderr(bool enabled);
// Tests only: shorten the repeat period.
void set_repeat_period(std::chrono::milliseconds period);
// With a file set, redirected stderr carries only warnings and errors; an
// attached console receives lines at the configured level immediately. Info and
// lower lines reach the file within the flush interval (250 ms) or with the
// next warning or error. flush() writes them out now; call it before an exit
// that skips static destructors.
void flush();
// Prepare before guest workers start. Terminal exit can then request a best-
// effort report/flush without entering shared logging or stdio locks itself.
void prepare_terminal_handoff();
void terminal_handoff(int status, std::chrono::milliseconds budget = std::chrono::milliseconds(50)) noexcept;
// Fatal host handler only: no mutex, formatting or allocation. Writes complete
// pending lines and the supplied already formatted bytes to the file and stderr.
void crash_write(const char* bytes, std::size_t size) noexcept;
// Tests only: change the flush interval.
void set_flush_interval(std::chrono::milliseconds interval);
void write(Level level, std::string_view tag, std::string_view message);

template <class... Args>
void trace(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
  if (level() <= Level::trace) write(Level::trace, tag, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void debug(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
  if (level() <= Level::debug) write(Level::debug, tag, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void info(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
  if (level() <= Level::info) write(Level::info, tag, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void warn(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
  if (level() <= Level::warn) write(Level::warn, tag, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void error(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
  write(Level::error, tag, std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace venture::log
