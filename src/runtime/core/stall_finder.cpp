#include "core/stall_finder.h"
#include "core/hle.h"
#include "core/hle_runtime.h"
#include "core/linker.h"
#include "core/stall_watch.h"
#include "core/thread.h"
#include "core/thread_api.h"
#include "core/thread_runtime.h"
#include "common/first_frame_progress.h"
#include "common/log.h"
#include "platform/memory.h"
#include "platform/process_info.h"
#include "platform/profile.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <ctime>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace venture::stall_finder {
namespace {
constexpr std::size_t kMaxThreads = 128, kFrames = 16, kStackShown = 4, kShown = 12;
// Calls in progress this long while frames keep coming get a "[stall-op]" line,
// at most one per kCallSpacingMs and kMaxCalls per run.
constexpr std::int64_t kLongCallMs = 500, kCallSpacingMs = 5000;
constexpr int kMaxCalls = 60;
// Host stacks: words copied per thread, frames kept, distinct stacks logged.
constexpr std::size_t kHostWords = 4096, kHostFrames = 10, kHostLines = 48;
constexpr std::int64_t kMs = 1'000'000;

struct Row {
  std::int32_t tid = 0;
  std::uint32_t host_tid = 0;
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
    row.host_tid = thread.host_tid.load(std::memory_order_relaxed);
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

std::string secs(std::int64_t ns) { return std::format("{:.2f} s", double(ns) / 1e9); }
std::string signed_secs(std::int64_t ns) { return std::format("{:+.2f} s", double(ns) / 1e9); }
const char* session_state_name(int state) {
  constexpr const char* names[] = {"unknown", "IDLE", "READY", "SYNCHRONIZED", "VISIBLE", "FOCUSED", "STOPPING",
                                   "LOSS_PENDING", "EXITING"};
  return state >= 0 && state <= 8 ? names[state] : "unknown";
}
std::string local_clock(std::chrono::system_clock::time_point at) {
  const auto time = std::chrono::system_clock::to_time_t(at);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &time);
#else
  localtime_r(&time, &tm);
#endif
  return std::format("{:02}:{:02}:{:02}", tm.tm_hour, tm.tm_min, tm.tm_sec);
}

// ---- host stacks ----------------------------------------------------------
// A word on a stack that points just past a call instruction is most likely a
// return address (the scan crash reporters fall back to without unwind data).
bool looks_like_return(std::uintptr_t address) {
#if defined(__aarch64__) || defined(_M_ARM64)
  std::uint32_t insn = 0;
  if (address < 4 || (address & 3) || !platform::read_memory(address - 4, &insn, 4)) return false;
  return (insn & 0xFC000000u) == 0x94000000u || (insn & 0xFFFFFC1Fu) == 0xD63F0000u;  // BL, BLR
#else
  std::array<std::uint8_t, 8> b{};
  if (address < 8 || !platform::read_memory(address - 8, b.data(), b.size())) return false;
  if (b[3] == 0xE8) return true;                                       // call rel32
  if (b[6] == 0xFF && (b[7] & 0xF8) == 0xD0) return true;              // call reg
  if (b[5] == 0xFF && (b[6] & 0xC0) == 0x40 && (b[6] & 0x38) == 0x10) return true;  // call [reg+disp8]
  if (b[2] == 0xFF && (b[3] & 0x38) == 0x10 && ((b[3] & 0xC7) == 0x05 || (b[3] & 0xC0) == 0x80))
    return true;                                                       // call [rip+disp32], [reg+disp32]
  if (b[1] == 0xFF && (b[2] & 0xC7) == 0x84 && (b[2] & 0x38) == 0x10) return true;  // call [sib+disp32]
  return false;
#endif
}
struct Modules {
  std::vector<platform::ProfileModule> list;  // by start address, exports sorted
  explicit Modules(std::vector<platform::ProfileModule> modules) : list(std::move(modules)) {
    std::ranges::sort(list, {}, &platform::ProfileModule::begin);
    for (auto& m : list) std::ranges::sort(m.exports, {}, &platform::ProfileSymbol::begin);
  }
  const platform::ProfileModule* find(std::uintptr_t address) const {
    auto it = std::ranges::upper_bound(list, address, {}, &platform::ProfileModule::begin);
    if (it == list.begin()) return nullptr;
    --it;
    return address < it->end ? &*it : nullptr;
  }
  std::string name(std::uintptr_t address) const {
    const auto* m = find(address);
    if (!m) return std::format("{:#x}", address);
    auto e = std::ranges::upper_bound(m->exports, address, {}, &platform::ProfileSymbol::begin);
    if (e != m->exports.begin() && address - std::prev(e)->begin < 0x10000)
      return std::format("{}!{}+{:#x}", m->name, std::prev(e)->name, address - std::prev(e)->begin);
    return std::format("{}+{:#x}", m->name, address - m->begin);
  }
};
struct HostStack {
  std::uint32_t tid = 0;
  std::string name, text;
};
// Every thread's host stack but the caller's: its instruction pointer, then the
// stack words that look like return addresses into loaded modules.
std::vector<HostStack> host_stacks() {
  std::vector<HostStack> out;
  const auto threads = platform::profile_threads();
  const Modules modules(platform::profile_modules());
  std::vector<std::uintptr_t> words(kHostWords);
  for (const auto& t : threads) {
    std::uintptr_t ip = 0;
    std::size_t count = 0;
    if (!platform::profile_host_stack(t, words, ip, count)) continue;
    HostStack s{t.id, t.name, modules.name(ip)};
    std::size_t frames = 1;
    std::uintptr_t previous = ip;
    for (std::size_t i = 0; i < count && frames < kHostFrames; ++i) {
      const auto w = words[i];
      if (w == previous || !modules.find(w) || !looks_like_return(w)) continue;
      s.text += " < " + modules.name(w);
      previous = w;
      ++frames;
    }
    out.push_back(std::move(s));
  }
  return out;
}

// What the stall watch, the GPU and the host showed at one moment of a stall.
struct Gathered {
  std::int64_t at_ns = 0;
  std::vector<stall_watch::ThreadView> threads;
  std::map<std::uint32_t, std::string> os_names;  // OS thread names
  int session_state = 0;
  std::int64_t gc_since_ns = 0;
  bool gpu_read = false, xr_lock_held = false;
  GpuEvidence gpu;
  std::vector<HostStack> host;
};
}  // namespace

struct Finder::Impl {
  Snapshot first, second;
  int snapshots = 0, next_id = 1;
  Counters before;
  std::int64_t start_ns = 0;  // the stall's start, on stall_watch's clock
  std::string clock;          // and in local time
  Gathered latest;
  int logged = 0;             // the id of the record logged while ongoing
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

  void gather(const Sources& sources, bool full) {
    latest = {};
    latest.at_ns = stall_watch::now_ns();
    latest.threads = stall_watch::threads();
    for (const auto& t : platform::profile_threads())
      if (!t.name.empty()) latest.os_names[t.id] = t.name;
    latest.session_state = startup_diagnostics::host_session_state();
    latest.gc_since_ns = stall_watch::totals().gc_since_ns;
    if (sources.xr_session_lock_held) latest.xr_lock_held = sources.xr_session_lock_held();
    if (!full) return;
    if (sources.gpu) {
      latest.gpu = sources.gpu();
      latest.gpu_read = true;
    }
    if (sources.host_stacks) latest.host = host_stacks();
  }

  // Names for the threads of the stall watch: guest names first.
  std::string thread_name(std::uint32_t os_tid, std::int32_t guest_tid,
                          const std::map<std::int32_t, std::string>& guest_names) const {
    if (guest_tid)
      if (auto it = guest_names.find(guest_tid); it != guest_names.end() && !it->second.empty()) return it->second;
    if (auto it = latest.os_names.find(os_tid); it != latest.os_names.end()) return it->second;
    return std::format("tid {}", os_tid);
  }
  static std::string object_text(const stall_watch::OpView& op) {
    if (!op.object) return {};
    switch (op.kind) {
      case stall_watch::Kind::wait: return std::format(" ({:#x})", op.object);
      case stall_watch::Kind::submit:
      case stall_watch::Kind::present:
      case stall_watch::Kind::idle: return std::format(" (queue {:#x})", op.object);
      default: return {};
    }
  }
  // How long a call seen in progress lasted: its completed event, else still
  // running now, else at least until the last look.
  static std::string duration(const stall_watch::OpView& op, std::int64_t now,
                              const std::vector<stall_watch::EventView>& xr,
                              const std::vector<stall_watch::EventView>& vk,
                              const std::vector<stall_watch::ThreadView>& fresh, std::int64_t seen) {
    for (const auto* events : {&xr, &vk})
      for (const auto& e : *events)
        if (e.start_ns == op.since_ns && e.name && op.name && !std::strcmp(e.name, op.name))
          return "for " + secs(e.ns);
    for (const auto& t : fresh)
      for (const auto& o : t.ops)
        if (o.since_ns == op.since_ns && o.name == op.name) return "for " + secs(now - op.since_ns) + " and still in it";
    return "for at least " + secs(seen - op.since_ns);
  }
  static std::string fence_text(std::uint64_t handle, std::int64_t stall_start) {
    const auto f = stall_watch::fence(handle);
    if (!f.known) return "no submission signalling it was seen (Venture keeps the last 1024 fences)";
    std::string out = std::format("submitted in guest submission #{} on queue {:#x} {} before the stall began",
                                  f.submit, f.queue, secs(stall_start - f.submitted_ns));
    if (f.issued_ns && f.deferred) out += std::format(", which Venture's queue dispatcher gave to the driver {} later",
                                                      secs(f.issued_ns - f.submitted_ns));
    else if (f.deferred) out += ", which Venture's queue dispatcher still held, never given to the driver";
    else out += ", straight to the driver";
    return out;
  }

  // The record's summary and evidence from the last look and what changed since.
  void evidence(stall::Record& r, std::int64_t now, const std::map<std::int32_t, std::string>& guest_names) {
    const auto stall_ns = now - start_ns;
    const auto xr = stall_watch::xr_calls();
    const auto vk = stall_watch::vulkan_events();
    const auto fresh = stall_watch::threads();
    std::vector<std::string> summary;
    auto add = [&](std::string kind, std::string text) { r.evidence.push_back({std::move(kind), std::move(text)}); };
    // Calls in progress at the last look, since before the stall or for most of it.
    for (const auto& t : latest.threads) {
      if (t.ops.empty() || latest.at_ns - t.ops.front().since_ns < 100 * kMs) continue;
      const auto name = thread_name(t.os_tid, t.guest_tid, guest_names);
      const auto role = stall::thread_role(name);
      std::string text = std::format("{}{} (tid {}{}): ", name, role.empty() ? "" : " (" + role + ")", t.os_tid,
                                     t.guest_tid ? std::format(", guest {}", t.guest_tid) : "");
      std::string first;
      for (std::size_t i = 0; i < t.ops.size(); ++i) {
        const auto& op = t.ops[i];
        const auto part = std::format("{}{} from {} {}", op.name ? op.name : "?", object_text(op),
                                      signed_secs(op.since_ns - start_ns),
                                      duration(op, now, xr, vk, fresh, latest.at_ns));
        text += (i ? " > " : "") + part;
        if (!i) first = part;
      }
      add("op", text);
      if (summary.size() < 4 && (!role.empty() || latest.at_ns - t.ops.front().since_ns >= stall_ns / 2))
        summary.push_back(std::format("{} was in {}", name, first));
      for (const auto& op : t.ops)
        if (op.kind == stall_watch::Kind::wait && op.object && op.name && std::string_view(op.name) == "vkWaitForFences")
          add("fence", std::format("{} waited for fence {:#x}: {}", name, op.object, fence_text(op.object, start_ns)));
    }
    // Fences a thread kept checking and found unsignalled.
    for (const auto& t : latest.threads)
      for (const auto& p : t.polls) {
        if (latest.at_ns - p.since_ns < 100 * kMs) continue;
        const auto name = thread_name(t.os_tid, t.guest_tid, guest_names);
        std::uint64_t checks = p.checks;
        bool still = false;
        for (const auto& f : fresh)
          if (f.os_tid == t.os_tid)
            for (const auto& q : f.polls)
              if (q.fence == p.fence && q.since_ns == p.since_ns) { checks = q.checks; still = true; }
        const auto text = std::format("{} checked fence {:#x} {} times from {} on and found it unsignalled{}; {}",
                                      name, p.fence, checks, signed_secs(p.since_ns - start_ns),
                                      still ? " every time, still when the stall was recorded"
                                            : " until about when frames resumed",
                                      fence_text(p.fence, start_ns));
        add("fence", text);
        if (summary.size() < 4)
          summary.push_back(std::format("{} polled fence {:#x} {} times, {} ({})", name, p.fence, checks,
                                        still ? "never signalled" : "signalled only as frames resumed",
                                        fence_text(p.fence, start_ns)));
      }
    // The last submits and waits per queue, relative to the stall's start.
    std::map<std::uint64_t, std::vector<const stall_watch::EventView*>> by_queue;
    for (const auto& e : vk) by_queue[e.queue].push_back(&e);
    for (const auto& [queue, events] : by_queue) {
      std::string text = queue ? std::format("queue {:#x}:", queue) : std::string("device waits:");
      const auto first = events.size() > 6 ? events.size() - 6 : 0;
      for (std::size_t i = first; i < events.size(); ++i) {
        const auto& e = *events[i];
        text += std::format("{} {}{}{} at {} ({:.2f} ms, result {})", i == first ? "" : ";", e.name ? e.name : "?",
                            e.submit ? std::format(" #{}", e.submit) : "",
                            e.object ? std::format(" {:#x}", e.object) : "", signed_secs(e.start_ns - start_ns),
                            double(e.ns) / 1e6, e.result);
      }
      add("queue", text);
    }
    // The GPU's position.
    if (latest.gpu_read) {
      if (!latest.gpu.summary.empty()) {
        add("gpu", latest.gpu.summary);
        summary.push_back(latest.gpu.summary);
      }
      for (std::size_t i = 0; i < latest.gpu.lines.size() && i < 6; ++i) add("gpu", latest.gpu.lines[i]);
    }
    // The XR session and its last calls.
    {
      std::string text = std::format("session {}; last calls:", session_state_name(latest.session_state));
      const auto first = xr.size() > 10 ? xr.size() - 10 : 0;
      for (std::size_t i = first; i < xr.size(); ++i)
        text += std::format("{} {} at {} ({:.2f} ms, result {})", i == first ? "" : ",", xr[i].name ? xr[i].name : "?",
                            signed_secs(xr[i].start_ns - start_ns), double(xr[i].ns) / 1e6, xr[i].result);
      if (xr.empty()) text += " none";
      add("xr", text);
    }
    // Held locks, and who was in a call on that queue.
    for (const auto& [queue, owner] : latest.gpu.held_queue_locks) {
      std::string holders;
      for (const auto& t : latest.threads)
        for (const auto& op : t.ops)
          if (op.object == queue && op.kind != stall_watch::Kind::xr && op.kind != stall_watch::Kind::wait)
            holders += std::format("{}{} in {}", holders.empty() ? "" : ", ", thread_name(t.os_tid, t.guest_tid, guest_names),
                                   op.name ? op.name : "?");
      add("lock", std::format("Vulkan queue {:#x} lock held (owner {}){}", queue, owner,
                              holders.empty() ? "" : "; in a call on that queue: " + holders));
    }
    if (latest.xr_lock_held) add("lock", "the XR session lock was held");
    if (latest.gc_since_ns && latest.gc_since_ns <= latest.at_ns) {
      add("gc", std::format("a Java garbage collection was running from {} on", signed_secs(latest.gc_since_ns - start_ns)));
      if (latest.at_ns - latest.gc_since_ns >= stall_ns / 2)
        summary.push_back("a Java garbage collection ran for " + secs(latest.at_ns - latest.gc_since_ns));
    }
    // Host stacks of the render-path threads and of threads in a call.
    std::map<std::uint32_t, std::string> key;
    for (unsigned i = 0; i < first.count; ++i) {
      const auto& row = first.rows[i];
      const auto it = guest_names.find(row.tid);
      if (row.host_tid && it != guest_names.end() && !stall::thread_role(it->second).empty()) key[row.host_tid] = it->second;
    }
    for (const auto& t : latest.threads)
      if (!t.ops.empty()) key.try_emplace(t.os_tid, thread_name(t.os_tid, t.guest_tid, guest_names));
    for (const auto& s : latest.host)
      if (auto it = key.find(s.tid); it != key.end()) add("host", std::format("{} (tid {}): {}", it->second, s.tid, s.text));
    for (std::size_t i = 0; i < summary.size(); ++i) r.summary += (i ? "; " : "") + summary[i];
  }

  // Every thread's host stack, identical stacks once: "[stall-host]" lines.
  void log_host_stacks(int id) const {
    std::map<std::string, std::vector<const HostStack*>> by_text;
    for (const auto& s : latest.host) by_text[s.text].push_back(&s);
    std::vector<std::pair<std::string, std::vector<const HostStack*>>> sorted(by_text.begin(), by_text.end());
    std::ranges::stable_sort(sorted, std::greater{}, [](const auto& p) { return p.second.size(); });
    std::size_t lines = 0;
    for (const auto& [text, threads] : sorted) {
      if (lines++ == kHostLines) {
        log::info("stall-host", "#{} {} more distinct stacks left out", id, sorted.size() - kHostLines);
        break;
      }
      std::string names;
      for (std::size_t i = 0; i < threads.size() && i < 6; ++i)
        names += std::format("{}{} {}", i ? ", " : "", threads[i]->tid, threads[i]->name);
      if (threads.size() > 6) names += std::format(", {} more", threads.size() - 6);
      log::info("stall-host", "#{} threads={} names=\"{}\" stack=\"{}\"", id, threads.size(), names, text);
    }
  }

  void record(std::int64_t now_ms, std::int64_t duration_ms, std::int64_t (*gc_ns)(), bool ongoing,
              std::string_view ended) {
    const auto after = read_counters(gc_ns);
    const auto now = stall_watch::now_ns();
    symbols.clear();
    stall::Record r;
    r.id = next_id++;
    r.at_s = (now_ms - duration_ms) / 1000.0;
    r.ms = duration_ms;
    r.clock = clock;
    r.ongoing = ongoing;
    r.ended = std::string(ended);
    r.snapshots = std::min(snapshots, 2);
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
    evidence(r, now, names);
    for (const auto& line : stall::format(r)) log::info("stall", "{}", line);
    if (!first.acquired) log::info("stall", "#{} thread registry busy at the snapshot", r.id);
    log_host_stacks(r.id);
    if (ongoing) logged = r.id;
  }
};

Finder::Finder(Sources sources) : sources_(sources) {}
Finder::~Finder() = default;

namespace {
// The watchdog for calls that run long while frames keep coming: the longest
// call in progress, Java GC and JIT compiles. One line each when they end (or
// the run ends), rate-limited.
struct LongCalls {
  int logged = 0;
  std::int64_t last_logged_ms = -kCallSpacingMs;
  struct Open { std::int64_t since_ns = 0; std::uint32_t tid = 0; std::int32_t guest = 0; std::string call; };
  std::optional<Open> open;
};
LongCalls& long_calls() {
  static LongCalls calls;
  return calls;
}
std::string thread_label(std::uint32_t os_tid, std::int32_t guest_tid) {
  if (guest_tid) {
    std::vector<std::pair<std::int32_t, std::string>> names;
    if (threads::try_list(names))
      for (const auto& [tid, name] : names)
        if (tid == guest_tid && !name.empty()) return name;
  }
  for (const auto& t : platform::profile_threads())
    if (t.id == os_tid && !t.name.empty()) return t.name;
  return std::format("tid {}", os_tid);
}
}  // namespace

void Finder::poll(std::int64_t now_ms, std::uint64_t frames, bool excluded) {
  const auto action = detector_.poll(now_ms, frames, excluded);
  // A call still open when frames stop belongs to the stall's record.
  if (action != stall::Detector::Action::none) long_calls().open.reset();
  switch (action) {
    case stall::Detector::Action::none: {
      if (excluded || now_ms < next_op_check_ms_) return;
      next_op_check_ms_ = now_ms + 100;
      auto& calls = long_calls();
      const auto now = stall_watch::now_ns();
      const auto longest = stall_watch::longest();
      const auto gc = stall_watch::totals().gc_since_ns;
      if (calls.open) {
        // Still the same call: wait for its end.
        if ((longest.since_ns == calls.open->since_ns && longest.os_tid == calls.open->tid) ||
            (calls.open->tid == 0 && gc == calls.open->since_ns))
          return;
        const stall::LongCall line{double(now_ms) / 1000.0 - double(now - calls.open->since_ns) / 1e9,
                                   (now - calls.open->since_ns) / kMs,
                                   calls.open->tid ? thread_label(calls.open->tid, calls.open->guest) : "Java VM",
                                   calls.open->call};
        log::info("stall-op", "{}", stall::format_call(line));
        calls.open.reset();
        return;
      }
      if (calls.logged >= kMaxCalls || now_ms - calls.last_logged_ms < kCallSpacingMs) return;
      if (longest.since_ns && now - longest.since_ns >= kLongCallMs * kMs) {
        std::string call = longest.name ? longest.name : "?";
        if (longest.object) call += std::format(" ({:#x})", longest.object);
        calls.open = LongCalls::Open{longest.since_ns, longest.os_tid, longest.guest_tid, std::move(call)};
      } else if (gc && now - gc >= kLongCallMs * kMs) {
        calls.open = LongCalls::Open{gc, 0, 0, "Java garbage collection"};
      } else {
        return;
      }
      ++calls.logged;
      calls.last_logged_ms = now_ms;
      return;
    }
    case stall::Detector::Action::snapshot: {
      if (!impl_) impl_ = std::make_unique<Impl>();
      take(impl_->first);
      impl_->before = read_counters(sources_.gc_ns);
      impl_->snapshots = 1;
      impl_->logged = 0;
      const auto stalled = detector_.stalled_ms(now_ms);
      impl_->start_ns = stall_watch::now_ns() - stalled * kMs;
      impl_->clock = local_clock(std::chrono::system_clock::now() - std::chrono::milliseconds(stalled));
      impl_->gather(sources_, false);
      return;
    }
    case stall::Detector::Action::recheck:
      take(impl_->second);
      impl_->snapshots = 2;
      impl_->gather(sources_, true);
      return;
    case stall::Detector::Action::ongoing:
      // Later than the recheck: compares the first snapshot with this one.
      take(impl_->second);
      impl_->snapshots = 3;
      impl_->gather(sources_, true);
      impl_->record(now_ms, detector_.duration_ms(), sources_.gc_ns, true, {});
      return;
    case stall::Detector::Action::record:
    case stall::Detector::Action::unfocused: {
      const std::string_view ended = action == stall::Detector::Action::unfocused ? "unfocused" : "";
      if (impl_->logged) log::info("stall", "{}", stall::format_end(impl_->logged, detector_.duration_ms(), ended));
      else impl_->record(now_ms, detector_.duration_ms(), sources_.gc_ns, false, ended);
      impl_->snapshots = 0;
      impl_->logged = 0;
      return;
    }
    case stall::Detector::Action::discard:
      impl_->snapshots = 0;
      return;
  }
}
}  // namespace venture::stall_finder
