#include "core/stall_finder.h"
#include "core/hle.h"
#include "core/hle_runtime.h"
#include "core/linker.h"
#include "core/thread.h"
#include "core/thread_api.h"
#include "core/thread_runtime.h"
#include "common/log.h"
#include "platform/memory.h"
#include "platform/process_info.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <map>
#include <string>
#include <vector>

namespace venture::stall_finder {
namespace {
constexpr std::size_t kMaxThreads = 128, kFrames = 16, kStackShown = 4, kShown = 12;
struct Row {
  std::int32_t tid = 0;
  GuestAddr pc = 0, lr = 0;
  std::uint32_t host_call = 0;  // HLE id + 1
  std::uint64_t compiling = 0;  // guest PC | 1
  std::array<GuestAddr, kFrames> frames{};
  unsigned count = 0;
  bool operator==(const Row& o) const {
    return pc == o.pc && lr == o.lr && host_call == o.host_call && compiling == o.compiling && count == o.count &&
           frames == o.frames;
  }
};
struct Snapshot {
  std::array<Row, kMaxThreads> rows;
  unsigned count = 0, dropped = 0;
  bool acquired = false;
  std::vector<std::pair<std::int32_t, std::string>> names;
  const Row* find(std::int32_t tid) const {
    for (unsigned i = 0; i < count; ++i)
      if (rows[i].tid == tid) return &rows[i];
    return nullptr;
  }
};
struct Counters {
  bool valid = false;
  platform::ProcessActivity activity;
  std::int64_t gc_ns = -1;
  std::chrono::steady_clock::time_point at;
};
Counters read_counters(std::int64_t (*gc_ns)()) {
  Counters c;
  c.at = std::chrono::steady_clock::now();
  c.valid = platform::process_activity(c.activity);
  c.gc_ns = gc_ns ? gc_ns() : -1;
  return c;
}
// The raw walk of signals.cpp's try_log_guest_threads_raw: saved registers and
// the frame-pointer chain, under a try-lock, copying only; named later.
void take(Snapshot& s) {
  s.count = s.dropped = 0;
  s.acquired = try_for_each_guest_thread([&](GuestThread& thread) {
    if (s.count == s.rows.size()) { ++s.dropped; return; }
    auto& row = s.rows[s.count++];
    row = {};
    auto& cpu = thread.cpu();
    row.tid = thread.tid(); row.pc = cpu.pc(); row.lr = cpu.lr();
    row.host_call = thread.host_call.load(std::memory_order_relaxed);
    row.compiling = thread.jit_compiling.load(std::memory_order_relaxed);
    if (cpu.isa() != GuestIsa::a64) return;
    auto fp = cpu.frame_pointer();
    while (fp && !(fp & 15) && row.count < row.frames.size()) {
      std::array<std::uint64_t, 2> record{};
      if (!platform::read_memory(fp, record.data(), sizeof(record))) break;
      row.frames[row.count++] = record[1];
      if (record[0] <= fp || record[0] - fp > 16 * 1024 * 1024) break;
      fp = record[0];
    }
  });
  if (!threads::try_list(s.names)) s.names.clear();
}
}  // namespace

struct Finder::Impl {
  Snapshot first, second;
  int snapshots = 0, next_id = 1;
  Counters before;
  // A linker busy loading gets one short wait per run, as in the raw dump.
  std::chrono::milliseconds linker_wait{250};
  std::map<GuestAddr, std::string> symbols;  // per record

  std::string symbol(GuestAddr pc) {
    if (const auto it = symbols.find(pc); it != symbols.end()) return it->second;
    std::string out;
    DlInfo info;
    const auto start = std::chrono::steady_clock::now();
    const bool found = Linker::get().try_address_info(pc, &info, linker_wait);
    if (!found && std::chrono::steady_clock::now() - start >= linker_wait) linker_wait = {};
    if (found && !info.path.empty()) {
      const auto slash = info.path.find_last_of('/');
      out = std::format("{}+{:#x}", info.path.substr(slash == std::string::npos ? 0 : slash + 1), pc - info.base);
      // Stripped libraries (IL2CPP) name the nearest export, often far away and misleading.
      if (!info.symbol.empty() && info.symbol_address <= pc && pc - info.symbol_address < 0x10000)
        out += std::format(" ({}+{:#x})", info.symbol, pc - info.symbol_address);
    } else if (const auto* hle = hle::function_at(pc)) {
      out = std::format("{}!{}", hle->library, hle->name);
    } else {
      out = std::format("{:#x}", pc);
    }
    return symbols[pc] = out;
  }
  static stall::Cause cause(const Row& row) {
    if (row.compiling) return stall::Cause::jit;
    if (row.host_call) {
      const auto& f = hle::function(row.host_call - 1);
      return stall::host_call_cause(f.library, f.name);
    }
    return stall::Cause::game;
  }
  std::string where(const Row& row) {
    if (row.compiling) return "JIT compile of " + symbol(row.compiling & ~std::uint64_t{1});
    if (row.host_call) {
      const auto& f = hle::function(row.host_call - 1);
      return f.library + "!" + f.name;
    }
    return symbol(row.pc);
  }
  std::string stack(const Row& row) {
    std::vector<GuestAddr> pcs;
    if (row.host_call) pcs.push_back(row.lr);  // the thunk's caller
    for (unsigned i = 0; i < row.count; ++i) pcs.push_back(row.frames[i]);
    std::string out;
    GuestAddr previous = 0;
    for (std::size_t i = 0, shown = 0; i < pcs.size() && shown < kStackShown; ++i) {
      if (!pcs[i] || pcs[i] == previous || pcs[i] == row.pc) continue;
      previous = pcs[i];
      out += (shown++ ? " < " : "") + symbol(pcs[i]);
    }
    return out;
  }

  void record(std::int64_t now_ms, std::int64_t duration_ms, std::int64_t (*gc_ns)()) {
    const auto after = read_counters(gc_ns);
    symbols.clear();
    stall::Record r;
    r.id = next_id++;
    r.at_s = (now_ms - duration_ms) / 1000.0;
    r.ms = duration_ms;
    r.snapshots = snapshots;
    r.threads = int(first.count + first.dropped);
    if (before.valid && after.valid) {
      const auto& a = after.activity;
      const auto& b = before.activity;
      if (const auto wall = std::chrono::duration<double>(after.at - before.at).count(); wall > 0)
        r.cpu_percent = (a.cpu_ns - b.cpu_ns) / (wall * 1e9) * 100;
      r.faults = std::int64_t(a.faults - b.faults);
      if (a.hard_faults >= 0 && b.hard_faults >= 0) r.hard_faults = a.hard_faults - b.hard_faults;
      r.read_kib = std::int64_t((a.read_bytes - b.read_bytes) / 1024);
    }
    if (before.gc_ns >= 0 && after.gc_ns >= 0) r.gc_ms = (after.gc_ns - before.gc_ns) / 1'000'000;
    if (platform::CommitCharge commit; platform::commit_charge(commit)) r.private_mib = std::int64_t(commit.process >> 20);
    std::map<std::int32_t, std::string> names(first.names.begin(), first.names.end());
    for (const auto& [tid, name] : second.names) names.try_emplace(tid, name);

    // Rank without symbols, then name only the threads shown.
    struct Candidate { int rank; const Row* row; const Row* later; std::string name, role; stall::Cause cause; };
    std::vector<Candidate> candidates;
    for (unsigned i = 0; i < first.count; ++i) {
      const auto& row = first.rows[i];
      // A host thread (the Android main thread, JVM threads) between guest calls
      // has no guest state to show; it is not running game code.
      if (!row.pc && !row.host_call && !row.compiling) continue;
      const Row* later = snapshots > 1 && second.acquired ? second.find(row.tid) : nullptr;
      Candidate c{0, &row, later, {}, {}, cause(row)};
      if (const auto it = names.find(row.tid); it != names.end()) c.name = it->second;
      c.role = stall::thread_role(c.name);
      const bool moved = later && !(*later == row);
      // Render-path threads first, then Venture work, then other host calls,
      // then game code that ran; threads idling in waits are left out.
      if (c.role == "main") c.rank = 0;
      else if (c.role == "render") c.rank = 1;
      else if (c.role == "rhi") c.rank = 2;
      else if (c.cause == stall::Cause::jit || c.cause == stall::Cause::pipeline) c.rank = 3;
      else if (c.cause != stall::Cause::wait && c.cause != stall::Cause::game) c.rank = 4;
      else if (c.cause == stall::Cause::game) c.rank = moved ? 5 : 6;
      else continue;
      candidates.push_back(std::move(c));
    }
    std::ranges::stable_sort(candidates, {}, &Candidate::rank);
    if (candidates.size() > kShown) candidates.resize(kShown);
    for (const auto& c : candidates) {
      stall::Thread t;
      t.tid = c.row->tid;
      t.name = c.name;
      t.role = c.role;
      t.cause = c.cause;
      t.where = where(*c.row);
      t.stack = stack(*c.row);
      if (c.later) {
        t.moved = *c.later == *c.row ? 0 : 1;
        // Where it got to by the second snapshot, when that is somewhere else.
        if (t.moved) {
          if (const auto then = where(*c.later); then != t.where) t.where += " -> " + then;
        }
      }
      r.shown.push_back(std::move(t));
    }
    r.cause = stall::classify(r);
    for (const auto& line : stall::format(r)) log::info("stall", "{}", line);
    if (!first.acquired) log::info("stall", "#{} thread registry busy at the snapshot", r.id);
  }
};

Finder::Finder(std::int64_t (*gc_ns)()) : gc_ns_(gc_ns) {}
Finder::~Finder() = default;

void Finder::poll(std::int64_t now_ms, std::uint64_t frames, bool excluded) {
  switch (detector_.poll(now_ms, frames, excluded)) {
    case stall::Detector::Action::none:
      return;
    case stall::Detector::Action::snapshot:
      if (!impl_) impl_ = std::make_unique<Impl>();
      take(impl_->first);
      impl_->before = read_counters(gc_ns_);
      impl_->snapshots = 1;
      return;
    case stall::Detector::Action::recheck:
      take(impl_->second);
      impl_->snapshots = 2;
      return;
    case stall::Detector::Action::record:
      impl_->record(now_ms, detector_.duration_ms(), gc_ns_);
      impl_->snapshots = 0;
      return;
    case stall::Detector::Action::discard:
      impl_->snapshots = 0;
      return;
  }
}
}  // namespace venture::stall_finder
