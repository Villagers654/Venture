#include "common/log.h"
#include "common/paths.h"
#ifdef __ANDROID__
#include <android/log.h>
#endif

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
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
#include <condition_variable>
#include <mutex>
#include <future>
#include <string>
#include <thread>
#include <unordered_map>

namespace venture::log {
namespace {
using Clock = std::chrono::steady_clock;
std::atomic<Level> g_level{Level::info};
// Never destroyed: the detached flusher and other threads may still log
// during static destruction.
std::mutex &g_mutex = *new std::mutex;
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
// Lines for the file wait in one of two buffers, and only the writer thread
// writes the file: a thread that logs never waits for the disk. A game that
// logged 30 MB/s made every logging thread, its render thread among them,
// queue for a 64 KiB buffer's synchronous writes behind one lock, and froze
// for a second whenever a write stalled.
// Publish only complete lines. A fatal handler must not wait for a mutex held
// by the faulting thread or enter the C++ stream/allocator implementation.
constexpr std::size_t kBuffer = std::size_t(1) << 20;
std::atomic<char> g_buffers[2][kBuffer];
std::atomic<std::size_t> g_sizes[2]{};
std::atomic<int> g_active{0};    // the buffer lines go to
std::atomic<int> g_writing{-1};  // the buffer being written, or -1
// Serializes file writes, set_file and flush; taken before g_mutex.
std::mutex &g_io_mutex = *new std::mutex;
std::condition_variable &g_wake = *new std::condition_variable;
bool g_wake_now = false;  // requires g_mutex
// A run's log stops taking info and lower lines past the limit, and warnings
// and errors past twice it. Requires g_mutex, like the counts below.
std::uint64_t g_limit = std::uint64_t(256) << 20;
std::uint64_t g_buffered = 0;  // bytes taken for the current file
std::uint64_t g_capped[2]{};   // info and lower, and warning or error lines left out past the limit
std::uint64_t g_dropped = 0;   // lines left out while both buffers were full
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
void write_buffer(int index) noexcept {
  const auto size = g_sizes[index].load(std::memory_order_acquire);
  char chunk[4096];
  for (std::size_t at = 0; at < size;) {
    const auto count = std::min(size - at, sizeof(chunk));
    for (std::size_t i = 0; i < count; ++i)
      chunk[i] = g_buffers[index][at + i].load(std::memory_order_relaxed);
    raw_write(g_file, chunk, count);
    at += count;
  }
}
// Requires g_io_mutex and not g_mutex: writes the active buffer out, taking
// new lines in the other one meanwhile.
void drain() noexcept {
  int index;
  {
    std::lock_guard lock(g_mutex);
    if (g_file < 0 || g_crashing.load(std::memory_order_acquire)) return;
    index = g_active.load(std::memory_order_relaxed);
    if (!g_sizes[index].load(std::memory_order_relaxed)) return;
    g_writing.store(index, std::memory_order_release);
    g_active.store(index ^ 1, std::memory_order_release);
  }
  write_buffer(index);
  if (g_crashing.load(std::memory_order_acquire)) return;
  g_sizes[index].store(0, std::memory_order_release);
  g_writing.store(-1, std::memory_order_release);
}
// Requires g_mutex. Appends whole text to the active buffer; false if it is full.
bool push(std::string_view text) {
  const int index = g_active.load(std::memory_order_relaxed);
  const auto size = g_sizes[index].load(std::memory_order_relaxed);
  if (text.size() > kBuffer - size) return false;
  for (std::size_t i = 0; i < text.size(); ++i)
    g_buffers[index][size + i].store(text[i], std::memory_order_relaxed);
  g_sizes[index].store(size + text.size(), std::memory_order_release);
  g_buffered += text.size();
  if (size + text.size() > kBuffer / 2) g_wake_now = true;
  return true;
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
// With a log file, lines reach it within the interval, and warnings and
// errors at once (the writer wakes for them).
std::chrono::milliseconds g_flush_interval{250};

std::string format_line(Level level, std::string_view tag, std::string_view message, Clock::time_point clock,
                        std::uint64_t repeats = 0, double since = 0) {
  auto now = std::chrono::duration_cast<std::chrono::milliseconds>(clock.time_since_epoch()).count();
  auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id()) % 100000;
  return repeats ? std::format("{:>10}.{:03} {:>5} {} [{}] {} ({} times in the last {:.1f} s)\n", now / 1000,
                               now % 1000, tid, kNames[static_cast<int>(level)], tag, message, repeats, since)
                 : std::format("{:>10}.{:03} {:>5} {} [{}] {}\n", now / 1000, now % 1000, tid,
                               kNames[static_cast<int>(level)], tag, message);
}

// Requires g_mutex. Takes a line for the file, or counts it left out.
void file_line(Level level, std::string_view line, Clock::time_point clock) {
  const bool important = level >= Level::warn;
  if (g_buffered + line.size() > (important ? 2 * g_limit : g_limit)) {
    if (!g_capped[important]++)
      push(format_line(Level::warn, "log",
                       std::format("The log file reached {} MiB this run: {} written no more", g_limit >> 20,
                                   important ? "warnings and errors are" : "info and lower lines are"),
                       clock));
    g_wake_now = true;
    return;
  }
  if (g_dropped &&
      push(format_line(Level::warn, "log",
                       std::format("{} lines left out: the log writer fell behind", g_dropped), clock)))
    g_dropped = 0;
  // A line longer than a buffer keeps its start.
  if (!push(line.substr(0, kBuffer)))
    ++g_dropped;
  if (important) g_wake_now = true;
  if (g_wake_now) g_wake.notify_one();
}

// Requires g_mutex.
void emit(Level level, std::string_view tag, std::string_view message, Clock::time_point clock,
          std::uint64_t repeats = 0, double since = 0) {
  std::string line = format_line(level, tag, message, clock, repeats, since);
  const bool important = level >= Level::warn;
#ifdef __ANDROID__
  // Android discards an app's stderr: lines at the log level also go to logcat.
  if (level >= g_level.load()) {
    const int priority = level >= Level::error ? ANDROID_LOG_ERROR
                         : level >= Level::warn ? ANDROID_LOG_WARN
                         : level >= Level::info ? ANDROID_LOG_INFO
                                                : ANDROID_LOG_DEBUG;
    __android_log_print(priority, "Venture", "[%.*s] %.*s", static_cast<int>(tag.size()), tag.data(),
                        static_cast<int>(message.size()), message.data());
  }
#endif
  if (g_console.stream && level >= g_level.load()) {
    std::fwrite(line.data(), 1, line.size(), g_console.stream);
    std::fflush(g_console.stream); // Console output must be live, not file-buffered.
  }
  if (g_file >= 0) {
    if (g_crashing.load(std::memory_order_acquire)) return;
    file_line(level, line, clock);
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

// Writes the pending repeat counts and every line taken so far.
void flush_all() {
  std::lock_guard io(g_io_mutex);
  {
    std::lock_guard lock(g_mutex);
    flush_repeats();
  }
  drain();
  {
    // Lines left out at the end are still counted.
    std::lock_guard lock(g_mutex);
    if (!g_dropped || g_file < 0 ||
        !push(format_line(Level::warn, "log", std::format("{} lines left out: the log writer fell behind", g_dropped),
                          Clock::now())))
      return;
    g_dropped = 0;
  }
  drain();
}
struct ExitFlush {
  ~ExitFlush() { flush_all(); }
} g_exit_flush; // Destroyed before g_file.
// Buffered lines reach the file within the flush interval even when nothing is
// logged after them. Flushing only on the next line kept a stalled process's
// last lines in memory: its log ended up to an interval before the stall, so a
// hang read as one at an earlier step (issue 23's runs "stopped" at HotSpot's
// start).
void start_writer() {
  static std::once_flag started;
  std::call_once(started, [] {
    std::thread([] {
      for (;;) {
        {
          std::unique_lock lock(g_mutex);
          const auto wait = std::clamp(g_flush_interval, std::chrono::milliseconds(1), std::chrono::milliseconds(250));
          g_wake.wait_for(lock, wait, [] { return g_wake_now; });
          g_wake_now = false;
        }
        std::lock_guard io(g_io_mutex);
        drain();
      }
    }).detach();
  });
}
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

void flush() { flush_all(); }
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
  // The first call writes the buffers, the one being written first (some of
  // its lines may then appear twice); subsequent frame lines append directly.
  if (!g_crashing.exchange(true, std::memory_order_acq_rel) && g_file >= 0) {
    const int writing = g_writing.load(std::memory_order_acquire);
    if (writing >= 0) write_buffer(writing);
    write_buffer(g_active.load(std::memory_order_acquire));
  }
  if (g_file >= 0) raw_write(g_file, bytes, size);
  raw_write(2, bytes, size);
}

namespace {
char g_fatal_reason[1024];
std::atomic<int> g_fatal_reason_state{0};  // 0 empty, 1 writing, 2 set
}  // namespace
void set_fatal_reason(std::string_view reason) noexcept {
  int expected = 0;
  if (!g_fatal_reason_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) return;
  const auto size = std::min(reason.size(), sizeof(g_fatal_reason) - 1);
  std::memcpy(g_fatal_reason, reason.data(), size);
  g_fatal_reason[size] = '\0';
  g_fatal_reason_state.store(2, std::memory_order_release);
}
const char* fatal_reason() noexcept {
  return g_fatal_reason_state.load(std::memory_order_acquire) == 2 ? g_fatal_reason : "";
}

void set_flush_interval(std::chrono::milliseconds interval) {
  std::lock_guard lock(g_mutex);
  g_flush_interval = interval;
  g_wake_now = true;
  g_wake.notify_one();
}

void set_file_limit(std::uint64_t bytes) {
  std::lock_guard lock(g_mutex);
  g_limit = bytes;
}

void set_level(Level level) { g_level = level; }
Level level() { return g_level; }

void set_stderr(bool enabled) { g_stderr.store(enabled, std::memory_order_relaxed); }

void set_repeat_period(std::chrono::milliseconds period) {
  std::lock_guard lock(g_mutex);
  g_repeat_period = period;
}

void set_file(std::string_view path) {
  // Pending counts belong to the old destination; the new one gets its own
  // first occurrences.
  std::lock_guard io(g_io_mutex);
  {
    std::lock_guard lock(g_mutex);
    flush_repeats();
  }
  drain();
  std::lock_guard lock(g_mutex);
  // Lines taken since the drain still belong to the old file.
  if (g_file >= 0 && !g_crashing.load(std::memory_order_acquire)) {
    const int index = g_active.load(std::memory_order_relaxed);
    write_buffer(index);
    g_sizes[index].store(0, std::memory_order_release);
  }
  if (g_file >= 0) VENTURE_LOG_CLOSE(g_file);
  g_file = -1;
  g_path = path;
  g_buffered = g_capped[0] = g_capped[1] = g_dropped = 0;
  if (!path.empty()) {
#ifdef _WIN32
    g_file = _wopen(paths::from_utf8(path).c_str(), _O_WRONLY | _O_CREAT | _O_APPEND | _O_BINARY,
                    _S_IREAD | _S_IWRITE);
    if (g_file >= 0) g_native_file = reinterpret_cast<HANDLE>(_get_osfhandle(g_file));
#else
    g_file = ::open(paths::from_utf8(path).c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
#endif
    if (g_file >= 0) start_writer();
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
