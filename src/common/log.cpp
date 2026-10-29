#include "common/log.h"
#include "common/paths.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#define VENTURE_LOG_CLOSE _close
#else
#include <unistd.h>
#define VENTURE_LOG_WRITE ::write
#define VENTURE_LOG_CLOSE ::close
#endif
#include <mutex>
#include <future>
#include <string>
#include <thread>
#include <unordered_map>

namespace venture::log {
namespace {
using Clock = std::chrono::steady_clock;
std::atomic<Level> g_level{Level::info};
std::mutex g_mutex;
struct TerminalHandoff {
  std::atomic<int> requested{-1};
  std::promise<void> completed;
  std::future<void> completion = completed.get_future();
};
std::atomic<TerminalHandoff*> g_terminal_handoff{nullptr};
int g_file = -1;
std::string g_path;
#ifdef _WIN32
HANDLE g_native_file = INVALID_HANDLE_VALUE;
#endif
// Publish only complete lines. A fatal handler must not wait for a mutex held
// by the faulting thread or enter the C++ stream/allocator implementation.
std::atomic<char> g_pending[65536];
std::atomic<std::size_t> g_pending_size{0};
std::atomic<bool> g_crashing{false};
std::atomic<bool> g_stderr{true};
static_assert(std::atomic<std::size_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(std::atomic<char>::is_always_lock_free);
void raw_write(int fd, const char* bytes, std::size_t size) noexcept {
  while (size) {
#ifdef _WIN32
    DWORD written = 0;
    OVERLAPPED append{};
    append.Offset = append.OffsetHigh = MAXDWORD;
    const auto handle = fd == g_file ? g_native_file : GetStdHandle(STD_ERROR_HANDLE);
    if (!WriteFile(handle, bytes, static_cast<DWORD>(size), &written,
                   fd == g_file ? &append : nullptr)) return;
    auto n = written;
#else
    auto n = VENTURE_LOG_WRITE(fd, bytes, static_cast<unsigned>(size));
    if (n < 0 && errno == EINTR) continue;
#endif
    if (n <= 0) return;
    bytes += n;
    size -= n;
  }
}
void write_pending() noexcept {
  const auto size = g_pending_size.load(std::memory_order_acquire);
  char chunk[4096];
  for (std::size_t at = 0; at < size;) {
    const auto count = std::min(size - at, sizeof(chunk));
    for (std::size_t i = 0; i < count; ++i)
      chunk[i] = g_pending[at + i].load(std::memory_order_relaxed);
    raw_write(g_file, chunk, count);
    at += count;
  }
}
void flush_file() noexcept {
  if (g_file < 0) return;
  write_pending();
  if (!g_crashing.load(std::memory_order_acquire))
    g_pending_size.store(0, std::memory_order_release);
}
struct Console {
  std::FILE* stream = nullptr;
  ~Console() { if (stream && stream != stderr) std::fclose(stream); }
} g_console;
constexpr const char *kNames[] = {"T", "D", "I", "W", "E"};
// A warning or error repeated with identical tag and text (e.g. a status query
// the runtime cannot answer, issued every frame) is written once. Later copies
// only count; the count is written with the message at most once per repeat
// period, and for every message still counting when the log file is closed or
// replaced and at process exit. A line "<message> (N times in the last S s)"
// stands for N occurrences. The first occurrence of every distinct message is
// always written; a full table is written out and starts over rather than
// suppressing new messages.
std::chrono::milliseconds g_repeat_period{10'000};
constexpr std::size_t kRepeatKeys = 1024;
struct Repeat {
  Clock::time_point reported, last;  // last report, last occurrence
  std::uint64_t suppressed = 0;
};
// Never destroyed: threads may still log during static destruction.
auto &g_repeats = *new std::unordered_map<std::string, Repeat>;
// With a log file, info and lower lines are buffered: the file is flushed on
// every warning or error and otherwise at most once per interval.
std::chrono::milliseconds g_flush_interval{250};
Clock::time_point g_flushed;

// Requires g_mutex.
void emit(Level level, std::string_view tag, std::string_view message, Clock::time_point clock,
          std::uint64_t repeats = 0, double since = 0) {
  auto now = std::chrono::duration_cast<std::chrono::milliseconds>(clock.time_since_epoch()).count();
  auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id()) % 100000;
  std::string line =
      repeats ? std::format("{:>10}.{:03} {:>5} {} [{}] {} ({} times in the last {:.1f} s)\n", now / 1000,
                            now % 1000, tid, kNames[static_cast<int>(level)], tag, message, repeats, since)
              : std::format("{:>10}.{:03} {:>5} {} [{}] {}\n", now / 1000, now % 1000, tid,
                            kNames[static_cast<int>(level)], tag, message);
  const bool important = level >= Level::warn;
  if (g_console.stream && level >= g_level.load()) {
    std::fwrite(line.data(), 1, line.size(), g_console.stream);
    std::fflush(g_console.stream); // Console output must be live, not file-buffered.
  }
  if (g_file >= 0) {
    if (g_crashing.load(std::memory_order_acquire)) return;
    auto pending = g_pending_size.load(std::memory_order_relaxed);
    if (line.size() > sizeof(g_pending) - pending) {
      flush_file();
      pending = 0;
    }
    if (g_crashing.load(std::memory_order_acquire)) return;
    if (line.size() > sizeof(g_pending)) {
      raw_write(g_file, line.data(), line.size());
    } else {
      for (std::size_t i = 0; i < line.size(); ++i)
        g_pending[pending + i].store(line[i], std::memory_order_relaxed);
      g_pending_size.store(pending + line.size(), std::memory_order_release);
    }
    if (important || clock - g_flushed >= g_flush_interval) {
      flush_file();
      g_flushed = clock;
    }
    // The launcher captures stderr into its own console log; with a file set,
    // stderr carries only warnings and errors instead of a second full copy.
    if (!important || !g_stderr.load(std::memory_order_relaxed))
      return;
  }
  if (g_console.stream == stderr)
    return; // An unredirected console already received this line.
  std::fwrite(line.data(), 1, line.size(), stderr);
}

// Requires g_mutex. Writes the count of every message suppressed since its last
// report (a burst that stopped) and forgets all messages.
void flush_repeats() {
  const auto clock = Clock::now();
  for (const auto &[key, repeat] : g_repeats) {
    if (!repeat.suppressed)
      continue;
    // Key: level digit, tag, NUL, message.
    const auto split = key.find('\0', 1);
    emit(static_cast<Level>(key[0] - '0'), std::string_view(key).substr(1, split - 1),
         std::string_view(key).substr(split + 1), clock, repeat.suppressed,
         std::chrono::duration<double>(clock - repeat.reported).count());
  }
  g_repeats.clear();
}

// Requires g_mutex. Writes the count of every message not seen for a full
// period and forgets those messages, so a burst that stopped is reported
// within two periods even if the process is then terminated without running
// exit handlers (the supervisor's job object, _Exit). A burst still going
// keeps its periodic report in write().
Clock::time_point g_swept;
void sweep_repeats(Clock::time_point clock) {
  for (auto it = g_repeats.begin(); it != g_repeats.end();) {
    const auto &[key, repeat] = *it;
    if (clock - repeat.last < g_repeat_period) {
      ++it;
      continue;
    }
    if (repeat.suppressed) {
      const auto split = key.find('\0', 1);
      emit(static_cast<Level>(key[0] - '0'), std::string_view(key).substr(1, split - 1),
           std::string_view(key).substr(split + 1), clock, repeat.suppressed,
           std::chrono::duration<double>(clock - repeat.reported).count());
    }
    it = g_repeats.erase(it);
  }
}

struct ExitFlush {
  ~ExitFlush() {
    std::lock_guard lock(g_mutex);
    flush_repeats();
    flush_file();
  }
} g_exit_flush; // Destroyed before g_file and g_mutex.
// Games log the Meta access tokens they are handed (FRL... user tokens and
// OC|app|secret app tokens); no log line keeps one.
bool token_char(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}
std::string_view redact_tokens(std::string_view message, std::string &out) {
  if (message.find("FRL") == std::string_view::npos && message.find("OC|") == std::string_view::npos)
    return message;
  out.clear();
  size_t i = 0;
  while (i < message.size()) {
    size_t body = std::string_view::npos;
    if (message.compare(i, 3, "FRL") == 0) {
      body = i + 3;
    } else if (message.compare(i, 3, "OC|") == 0) {
      size_t j = i + 3;
      while (j < message.size() && message[j] >= '0' && message[j] <= '9') ++j;
      if (j > i + 3 && j < message.size() && message[j] == '|') body = j + 1;
    }
    if (body != std::string_view::npos && (i == 0 || !token_char(message[i - 1]))) {
      size_t end = body;
      while (end < message.size() && token_char(message[end])) ++end;
      if (end - body >= 20) {
        out += "<redacted-token>";
        i = end;
        continue;
      }
    }
    out += message[i++];
  }
  return out;
}

} // namespace

void attach_console() {
#ifdef _WIN32
  std::lock_guard lock(g_mutex);
  if (g_console.stream)
    return;
  DWORD mode = 0;
  DWORD written = 0;
  const auto error = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stderr)));
  // GetConsoleMode needs read access; stderr can be a write-only console handle.
  if (GetConsoleMode(error, &mode) || WriteConsoleW(error, L"", 0, &written, nullptr))
    g_console.stream = stderr;
  else
    g_console.stream = std::fopen("CONOUT$", "w");
#endif
}

void flush() {
  std::lock_guard lock(g_mutex);
  flush_repeats();
  flush_file();
}
void prepare_terminal_handoff() {
  // Process-lifetime worker: never join it during exit, because an unrelated
  // writer can own the logging/stdio locks indefinitely. Allocation and thread
  // creation occur before entering guest code, not on the terminal path.
  static const auto prepared = [] {
    auto* handoff = new TerminalHandoff;
    std::thread([handoff] {
      handoff->requested.wait(-1);
      try {
        info("launch", "Terminal guest process exit status={}; releasing worker through process exit", handoff->requested.load());
        flush();
      } catch (...) {}
      handoff->completed.set_value();
    }).detach();
    g_terminal_handoff.store(handoff, std::memory_order_release);
    return true;
  }();
  (void)prepared;
}
void terminal_handoff(int status, std::chrono::milliseconds budget) noexcept {
  auto* handoff = g_terminal_handoff.load(std::memory_order_acquire);
  if (!handoff) return;
  handoff->requested.store(status);
  handoff->requested.notify_one();
  try { (void)handoff->completion.wait_for(budget); } catch (...) {}
}

void crash_write(const char* bytes, std::size_t size) noexcept {
  // Normal console mirrors flush each line. Do not enter their stdio locks
  // here: the faulting thread may hold one. Fatal output uses native writes.
  // The first call drains the buffer; subsequent frame lines append directly.
  if (!g_crashing.exchange(true, std::memory_order_acq_rel) && g_file >= 0)
    write_pending();
  if (g_file >= 0) raw_write(g_file, bytes, size);
  raw_write(2, bytes, size);
}

void set_flush_interval(std::chrono::milliseconds interval) {
  std::lock_guard lock(g_mutex);
  g_flush_interval = interval;
}

void set_level(Level level) { g_level = level; }
Level level() { return g_level; }

void set_stderr(bool enabled) { g_stderr.store(enabled, std::memory_order_relaxed); }

void set_repeat_period(std::chrono::milliseconds period) {
  std::lock_guard lock(g_mutex);
  g_repeat_period = period;
}

void set_file(std::string_view path) {
  std::lock_guard lock(g_mutex);
  // Pending counts belong to the old destination; the new one gets its own
  // first occurrences.
  flush_repeats();
  flush_file();
  if (g_file >= 0) VENTURE_LOG_CLOSE(g_file);
  g_file = -1;
  g_path = path;
  if (!path.empty()) {
#ifdef _WIN32
    g_file = _wopen(paths::from_utf8(path).c_str(), _O_WRONLY | _O_CREAT | _O_APPEND | _O_BINARY,
                    _S_IREAD | _S_IWRITE);
    if (g_file >= 0) g_native_file = reinterpret_cast<HANDLE>(_get_osfhandle(g_file));
#else
    g_file = ::open(paths::from_utf8(path).c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
#endif
  }
}

std::string file() {
  std::lock_guard lock(g_mutex);
  return g_path;
}

void write(Level level, std::string_view tag, std::string_view message) {
  std::string redacted;
  message = redact_tokens(message, redacted);
  const auto clock = Clock::now();
  std::lock_guard lock(g_mutex);
  if (clock - g_swept >= g_repeat_period) {
    sweep_repeats(clock);
    g_swept = clock;
  }
  std::uint64_t repeats = 0;
  double since = 0;
  if (level >= Level::warn) {
    std::string key;
    key.reserve(tag.size() + message.size() + 2);
    key.append(1, static_cast<char>('0' + static_cast<int>(level))).append(tag).append(1, '\0').append(message);
    if (auto it = g_repeats.find(key); it != g_repeats.end()) {
      auto &repeat = it->second;
      if (clock - repeat.reported < g_repeat_period) {
        ++repeat.suppressed;
        repeat.last = clock;
        return;
      }
      // A message slower than the period is written as it is.
      repeats = repeat.suppressed ? repeat.suppressed + 1 : 0;
      since = std::chrono::duration<double>(clock - repeat.reported).count();
      repeat = {clock, clock, 0};
    } else {
      if (g_repeats.size() >= kRepeatKeys)
        flush_repeats();
      g_repeats.emplace(std::move(key), Repeat{clock, clock, 0});
    }
  }
  emit(level, tag, message, clock, repeats, since);
}

} // namespace venture::log
