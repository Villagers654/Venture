#include "file_io_trace.h"
#include "vfs.h"
#include "common/environment.h"
#include "common/log.h"
#include "core/shutdown_trace.h"
#include "core/thread.h"
#include "core/wait.h"
#include <array>
#include <map>
#include <mutex>
#include <string>

namespace venture::hle::libc::file_io_trace {
namespace {
const char* const setting = environment::getenv("VENTURE_TRACE_FILE_IO");
constexpr std::int64_t slow_ns = 20'000'000;
constexpr std::size_t ring_size = 512, printed = 192;
struct Entry {
  std::int64_t end_ns{}, duration_ns{}, offset{}, result{};
  std::uint64_t n{};
  std::int32_t tid{}, fd{};
  char op{};
};
struct Totals {
  std::uint64_t calls{}, bytes{}, failures{};
  std::int64_t last_ns{};
};
struct State {
  std::mutex mutex;
  std::array<Entry, ring_size> ring{};
  std::uint64_t next = 0;
  std::map<std::int32_t, Totals> threads;
};
State& state() {
  static State s;
  return s;
}
std::int32_t current_tid() {
  auto* thread = GuestThread::current();
  return thread ? thread->tid() : 0;
}
void snapshot(const char* label) {
  auto& s = state();
  const auto now = monotonic_ns();
  for (const auto& [fd, path] : FdTable::get().paths())
    if (!path.empty()) log::info("file-io", "snapshot={} open fd={} path={}", label, fd, path);
  std::lock_guard lock(s.mutex);
  for (const auto& [tid, t] : s.threads)
    log::info("file-io", "snapshot={} tid={} calls={} bytes={} failures={} last_ms_ago={}", label, tid,
              t.calls, t.bytes, t.failures, (now - t.last_ns) / 1'000'000);
  const auto count = std::min<std::uint64_t>(s.next, printed);
  for (std::uint64_t i = s.next - count; i < s.next; ++i) {
    const auto& e = s.ring[i % ring_size];
    log::info("file-io", "snapshot={} recent ms_ago={} tid={} op={} fd={} n={} offset={} result={} us={}", label,
              (now - e.end_ns) / 1'000'000, e.tid, e.op, e.fd, e.n, e.offset, e.result, e.duration_ns / 1000);
  }
}
void record(char op, int fd, std::uint64_t n, std::int64_t offset, std::int64_t result, std::int64_t start_ns) {
  static const bool registered = [] { shutdown_trace::add_snapshot_source(snapshot); return true; }();
  (void)registered;
  const auto end = monotonic_ns();
  const auto tid = current_tid();
  auto& s = state();
  std::lock_guard lock(s.mutex);
  s.ring[s.next++ % ring_size] = {end, start_ns ? end - start_ns : 0, offset, result, n, tid, fd, op};
  auto& t = s.threads[tid];
  ++t.calls;
  if (result > 0 && op != 'm') t.bytes += static_cast<std::uint64_t>(result);
  if (result < 0) ++t.failures;
  t.last_ns = end;
}
}  // namespace
const bool enabled = setting && std::string_view(setting) == "1";
std::int64_t start() { return enabled ? monotonic_ns() : 0; }
void opened(std::string_view path, int flags, std::int64_t result) {
  record('o', result >= 0 ? static_cast<int>(result) : -1, 0, flags, result, 0);
  log::info("file-io", "open tid={} path={} flags={:#x} -> {}", current_tid(), path, flags, result);
}
void read(char op, int fd, std::string_view path, std::uint64_t n, std::int64_t offset,
          std::int64_t result, std::int64_t start_ns) {
  record(op, fd, n, offset, result, start_ns);
  const auto duration = monotonic_ns() - start_ns;
  // Would-block results are ordinary on pipes and sockets.
  if ((result < 0 && result != -err::eagain) || duration >= slow_ns)
    log::info("file-io", "{} tid={} fd={} path={} n={} offset={} -> {} us={}", op == 'p' ? "pread" : "read",
              current_tid(), fd, path, n, offset, result, duration / 1000);
}
void closed(int fd, std::int64_t result) { record('c', fd, 0, 0, result, 0); }
void mapped(int fd, std::string_view path, std::uint64_t size, std::int64_t offset, std::int64_t result) {
  record('m', fd, size, offset, result, 0);
  log::info("file-io", "mmap tid={} fd={} path={} size={} offset={} -> {:#x}", current_tid(), fd, path, size,
            offset, static_cast<std::uint64_t>(result));
}
}  // namespace venture::hle::libc::file_io_trace
