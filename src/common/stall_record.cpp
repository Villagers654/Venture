#include "common/stall_record.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <format>
#include <map>
#include <ranges>
#include <utility>

namespace venture::stall {
namespace {
constexpr std::array<std::pair<Cause, std::string_view>, 12> kCauseNames{{
    {Cause::jit, "jit"}, {Cause::pipeline, "pipeline"}, {Cause::gc, "gc"}, {Cause::paging, "paging"},
    {Cause::host, "host"}, {Cause::java, "java"}, {Cause::gpu, "gpu"}, {Cause::xr, "xr"},
    {Cause::io, "io"}, {Cause::wait, "wait"}, {Cause::game, "game"}, {Cause::unknown, "unknown"},
}};
std::string lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}
bool any_of(std::string_view name, std::initializer_list<std::string_view> names) {
  return std::ranges::find(names, name) != names.end();
}
// A quoted log value: quotes would end it early.
std::string quoted(std::string_view text, std::size_t limit = 160) {
  std::string out(text.substr(0, limit));
  std::ranges::replace(out, '"', '\'');
  return '"' + out + '"';
}
std::string_view role_rank_name(int rank) { return rank == 0 ? "main" : rank == 1 ? "render" : "rhi"; }
int role_rank(std::string_view role) { return role == "main" ? 0 : role == "render" ? 1 : role == "rhi" ? 2 : 3; }

// key=value and key="value" pairs of one record line.
std::map<std::string, std::string, std::less<>> fields(std::string_view text) {
  std::map<std::string, std::string, std::less<>> out;
  std::size_t at = 0;
  while (at < text.size()) {
    while (at < text.size() && text[at] == ' ') ++at;
    const auto eq = text.find('=', at), space = text.find(' ', at);
    if (eq == std::string_view::npos) break;
    if (space < eq) { at = space; continue; }  // a bare word
    std::string key(text.substr(at, eq - at));
    at = eq + 1;
    std::size_t end;
    if (at < text.size() && text[at] == '"') {
      end = text.find('"', ++at);
      if (end == std::string_view::npos) end = text.size();
      out[key] = std::string(text.substr(at, end - at));
      at = end + 1;
    } else {
      end = std::min(text.find(' ', at), text.size());
      out[key] = std::string(text.substr(at, end - at));
      at = end;
    }
  }
  return out;
}
template <typename T> T number(const std::map<std::string, std::string, std::less<>>& f, std::string_view key, T fallback) {
  const auto it = f.find(key);
  if (it == f.end()) return fallback;
  std::string_view value = it->second;
  while (!value.empty() && (value.back() == '%' || value.back() == 's')) value.remove_suffix(1);
  T out{};
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), out);
  return parsed.ec == std::errc{} ? out : fallback;
}
std::string seconds(std::int64_t ms) { return std::format("{:.2f} s", ms / 1000.0); }
}  // namespace

std::string thread_role(std::string_view input) {
  const auto value = lower(input);
  if (value == "unitymain" || value == "gamethread" || value == "mainthread" || value == "ue4gamethread") return "main";
  if (value.starts_with("rhithread")) return "rhi";
  if (value.starts_with("renderthread") || value == "unitygfxdevicew") return "render";
  return {};
}

std::string_view cause_name(Cause cause) {
  for (const auto& [c, name] : kCauseNames)
    if (c == cause) return name;
  return "unknown";
}
Cause cause_from_name(std::string_view name) {
  for (const auto& [c, n] : kCauseNames)
    if (n == name) return c;
  return Cause::unknown;
}
bool venture_side(Cause cause) {
  return cause == Cause::jit || cause == Cause::pipeline || cause == Cause::gc || cause == Cause::paging ||
         cause == Cause::host;
}

Cause host_call_cause(std::string_view library, std::string_view name) {
  if (library == "JNIEnv" || library == "JavaVM") return Cause::java;
  if (name.starts_with("vkCreateGraphicsPipelines") || name.starts_with("vkCreateComputePipelines") ||
      name.starts_with("vkCreateRayTracingPipelines") || name == "vkCreateShaderModule" ||
      any_of(name, {"glLinkProgram", "glCompileShader", "glProgramBinary", "glShaderBinary", "glProgramBinaryOES"}))
    return Cause::pipeline;
  if (any_of(name, {"vkWaitForFences", "vkQueueWaitIdle", "vkDeviceWaitIdle", "vkWaitSemaphores", "vkWaitSemaphoresKHR",
                    "vkQueueSubmit", "vkQueueSubmit2", "vkQueueSubmit2KHR", "vkQueuePresentKHR",
                    "vkAcquireNextImageKHR", "vkGetQueryPoolResults", "vkGetFenceStatus", "glFinish", "glFlush",
                    "glClientWaitSync", "glWaitSync", "eglSwapBuffers", "eglClientWaitSyncKHR", "glReadPixels"}))
    return Cause::gpu;
  if (library == "libopenxr_loader.so" || name.starts_with("xr")) return Cause::xr;
  if (any_of(name, {"read", "pread", "pread64", "readv", "preadv", "write", "pwrite", "pwrite64", "writev", "open",
                    "open64", "openat", "fopen", "fopen64", "fread", "fwrite", "fgets", "fflush", "fsync", "fdatasync",
                    "stat", "stat64", "lstat", "fstat", "fstatat", "access", "opendir", "readdir", "rename",
                    "unlink", "mkdir", "close", "fclose", "AAsset_read", "AAsset_getBuffer", "AAssetManager_open",
                    "AAsset_seek", "AAsset_seek64", "sqlite3_step", "sqlite3_exec"}))
    return Cause::io;
  if (name.find("wait") != std::string_view::npos || name.find("Wait") != std::string_view::npos ||
      name.find("sleep") != std::string_view::npos || name.find("futex") != std::string_view::npos ||
      any_of(name, {"syscall", "poll", "ppoll", "select", "pselect", "epoll_pwait", "pthread_join",
                    "pthread_mutex_lock", "pthread_rwlock_rdlock", "pthread_rwlock_wrlock", "sched_yield",
                    "pause", "sigsuspend", "rt_sigsuspend", "ALooper_pollOnce", "ALooper_pollAll", "recv",
                    "recvfrom", "recvmsg", "accept", "connect"}))
    return Cause::wait;
  return Cause::host;
}

Cause classify(const Record& r) {
  if (r.gc_ms >= 0 && r.ms > 0 && r.gc_ms * 2 >= r.ms) return Cause::gc;
  std::vector<const Thread*> roles;
  for (const auto& t : r.shown)
    if (!t.role.empty()) roles.push_back(&t);
  std::ranges::stable_sort(roles, {}, [](const Thread* t) { return role_rank(t->role); });
  for (const auto* t : roles)
    if (t->cause == Cause::jit || t->cause == Cause::pipeline) return t->cause;
  // 500 hard faults a second is ~2 MB/s of 4 KiB page-ins at best: disk-bound.
  if (r.hard_faults >= 0 && r.ms > 0 && r.hard_faults * 1000 >= 500 * r.ms) return Cause::paging;
  // The main thread waiting hands the question to the render thread, and so on.
  for (const auto* t : roles)
    if (t->cause != Cause::wait && t->cause != Cause::unknown) return t->cause;
  for (const auto& t : r.shown)
    if (t.cause == Cause::jit || t.cause == Cause::pipeline || t.cause == Cause::gc) return t.cause;
  return roles.empty() ? Cause::unknown : Cause::wait;
}

std::vector<std::string> format(const Record& r) {
  std::vector<std::string> lines;
  std::string head = std::format("#{} at={:.1f}s ms={} cause={} snapshots={} threads={}", r.id, r.at_s, r.ms,
                                 cause_name(r.cause), r.snapshots, r.threads);
  if (r.cpu_percent >= 0) head += std::format(" cpu={:.0f}%", r.cpu_percent);
  if (r.faults >= 0) head += std::format(" faults={}", r.faults);
  if (r.hard_faults >= 0) head += std::format(" hard_faults={}", r.hard_faults);
  if (r.read_kib >= 0) head += std::format(" read_kib={}", r.read_kib);
  if (r.gc_ms >= 0) head += std::format(" gc_ms={}", r.gc_ms);
  if (r.private_mib >= 0) head += std::format(" private_mib={}", r.private_mib);
  if (!r.clock.empty()) head += " clock=" + r.clock;
  if (r.ongoing) head += " state=ongoing";
  if (!r.ended.empty()) head += " ended=" + r.ended;
  lines.push_back(std::move(head));
  if (!r.summary.empty()) lines.push_back(std::format("#{} summary={}", r.id, quoted(r.summary, 900)));
  for (const auto& e : r.evidence)
    lines.push_back(std::format("#{} evidence={} text={}", r.id, e.kind, quoted(e.text, 900)));
  for (const auto& t : r.shown) {
    std::string line = std::format("#{} tid={} name={}", r.id, t.tid, quoted(t.name, 32));
    if (!t.role.empty()) line += " role=" + t.role;
    line += std::format(" cause={}", cause_name(t.cause));
    if (t.moved >= 0) line += t.moved ? " moved=yes" : " moved=no";
    line += " where=" + quoted(t.where);
    if (!t.stack.empty()) line += " stack=" + quoted(t.stack, 480);
    lines.push_back(std::move(line));
  }
  return lines;
}

std::string format_end(int id, std::int64_t ms, std::string_view ended) {
  return std::format("#{} end ms={}{}{}", id, ms, ended.empty() ? "" : " ended=", ended);
}

std::string format_call(const LongCall& c) {
  return std::format("at={:.1f}s ms={} thread={} call={}", c.at_s, c.ms, quoted(c.thread, 64), quoted(c.call, 300));
}

std::vector<LongCall> parse_calls(std::string_view text) {
  std::vector<LongCall> calls;
  constexpr std::string_view tag = " [stall-op] ";
  for (std::size_t at = 0; (at = text.find(tag, at)) != std::string_view::npos;) {
    auto end = text.find('\n', at);
    if (end == std::string_view::npos) end = text.size();
    const auto f = fields(text.substr(at + tag.size(), end - at - tag.size()));
    at = end;
    if (!f.contains("ms") || !f.contains("call")) continue;
    LongCall c;
    c.at_s = number<double>(f, "at", 0);
    c.ms = number<std::int64_t>(f, "ms", 0);
    if (auto it = f.find("thread"); it != f.end()) c.thread = it->second;
    c.call = f.find("call")->second;
    calls.push_back(std::move(c));
  }
  return calls;
}

std::vector<Record> parse_log(std::string_view text) {
  std::vector<Record> records;
  constexpr std::string_view tag = " [stall] #";
  for (std::size_t at = 0; (at = text.find(tag, at)) != std::string_view::npos;) {
    auto end = text.find('\n', at);
    if (end == std::string_view::npos) end = text.size();
    std::string_view line = text.substr(at + tag.size(), end - at - tag.size());
    at = end;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    int id = 0;
    const auto parsed = std::from_chars(line.data(), line.data() + line.size(), id);
    if (parsed.ec != std::errc{}) continue;
    const std::string_view rest(parsed.ptr, line.data() + line.size());
    const auto f = fields(rest);
    // The end of a record logged while ongoing, which may have other records' lines before it.
    if (rest.starts_with(" end ")) {
      auto r = records.rbegin();
      while (r != records.rend() && r->id != id) ++r;
      if (r == records.rend()) continue;
      r->ms = number<std::int64_t>(f, "ms", r->ms);
      r->ongoing = false;
      if (auto it = f.find("ended"); it != f.end()) r->ended = it->second;
      continue;
    }
    if (f.contains("summary") || f.contains("evidence")) {
      if (records.empty() || records.back().id != id) continue;
      if (auto it = f.find("summary"); it != f.end()) records.back().summary = it->second;
      else if (auto text_it = f.find("text"); text_it != f.end())
        records.back().evidence.push_back({f.find("evidence")->second, text_it->second});
      continue;
    }
    if (f.contains("tid")) {
      if (records.empty() || records.back().id != id) continue;  // a header the log lost
      Thread t;
      t.tid = number<std::int32_t>(f, "tid", 0);
      if (auto it = f.find("name"); it != f.end()) t.name = it->second;
      if (auto it = f.find("role"); it != f.end()) t.role = it->second;
      if (auto it = f.find("cause"); it != f.end()) t.cause = cause_from_name(it->second);
      if (auto it = f.find("moved"); it != f.end()) t.moved = it->second == "yes" ? 1 : it->second == "no" ? 0 : -1;
      if (auto it = f.find("where"); it != f.end()) t.where = it->second;
      if (auto it = f.find("stack"); it != f.end()) t.stack = it->second;
      records.back().shown.push_back(std::move(t));
      continue;
    }
    if (!f.contains("ms")) continue;
    Record r;
    r.id = id;
    r.at_s = number<double>(f, "at", 0);
    r.ms = number<std::int64_t>(f, "ms", 0);
    r.snapshots = number<int>(f, "snapshots", 0);
    r.threads = number<int>(f, "threads", 0);
    if (auto it = f.find("cause"); it != f.end()) r.cause = cause_from_name(it->second);
    r.cpu_percent = number<double>(f, "cpu", -1);
    r.faults = number<std::int64_t>(f, "faults", -1);
    r.hard_faults = number<std::int64_t>(f, "hard_faults", -1);
    r.read_kib = number<std::int64_t>(f, "read_kib", -1);
    r.gc_ms = number<std::int64_t>(f, "gc_ms", -1);
    r.private_mib = number<std::int64_t>(f, "private_mib", -1);
    if (auto it = f.find("clock"); it != f.end()) r.clock = it->second;
    if (auto it = f.find("state"); it != f.end()) r.ongoing = it->second == "ongoing";
    if (auto it = f.find("ended"); it != f.end()) r.ended = it->second;
    records.push_back(std::move(r));
  }
  return records;
}

namespace {
std::string calls_section(const std::vector<LongCall>& calls) {
  if (calls.empty()) return {};
  constexpr std::size_t kShown = 10;
  std::vector<const LongCall*> longest;
  for (const auto& c : calls) longest.push_back(&c);
  std::ranges::stable_sort(longest, std::greater{}, &LongCall::ms);
  std::string out = std::format("Calls that took 500 ms or more while frames kept coming: {}{}\n", calls.size(),
                                calls.size() > kShown ? std::format(", longest {} shown", kShown) : std::string());
  for (std::size_t i = 0; i < longest.size() && i < kShown; ++i)
    out += std::format("  at {:.1f} s: {} ms, {}: {}\n", longest[i]->at_s, longest[i]->ms,
                       longest[i]->thread.empty() ? "(unnamed)" : longest[i]->thread, longest[i]->call);
  return out;
}
}  // namespace

std::string report_section(const std::vector<Record>& records, const std::vector<LongCall>& calls) {
  if (records.empty()) return calls.empty() ? std::string() : "\n=== Stalls (0) ===\n" + calls_section(calls);
  constexpr std::size_t kShown = 10, kWithEvidence = 5, kEvidenceLines = 12;
  std::vector<const Record*> worst;
  std::int64_t total = 0;
  std::map<Cause, int> causes;
  // Where the main thread (else the render thread) was, once per stall.
  std::map<std::string, int> places;
  for (const auto& r : records) {
    worst.push_back(&r);
    total += r.ms;
    ++causes[r.cause];
    for (int rank = 0; rank < 3; ++rank) {
      const auto t = std::ranges::find(r.shown, role_rank_name(rank), &Thread::role);
      if (t == r.shown.end()) continue;
      ++places[std::format("{} ({}): {}", t->name, t->role, t->where)];
      break;
    }
  }
  std::ranges::stable_sort(worst, std::greater{}, &Record::ms);
  std::string out = std::format("\n=== Stalls ({}{}) ===\n", records.size(),
                                records.size() > kShown ? std::format(", worst {} shown", kShown) : std::string());
  out += std::format("Frames stopped for 250 ms or more {} times while the game had focus: {} in all, worst {}.\n",
                     records.size(), seconds(total), seconds(worst.front()->ms));
  const auto freezes = std::ranges::count_if(records, [](const Record& r) { return r.ms >= freeze_ms; });
  const auto frozen_at_end = std::ranges::count_if(records, &Record::ongoing);
  if (freezes)
    out += std::format("{} of them {} of 1 s or more{}.\n", freezes, freezes == 1 ? "was a freeze" : "were freezes",
                       frozen_at_end ? std::format(" ({} still frozen when the log ended)", frozen_at_end) : "");
  std::vector<std::pair<int, Cause>> by_count;
  for (const auto& [cause, count] : causes) by_count.emplace_back(count, cause);
  std::ranges::stable_sort(by_count, std::greater{}, &std::pair<int, Cause>::first);
  out += "Causes:";
  for (std::size_t i = 0; i < by_count.size(); ++i)
    out += std::format("{} {}{} {}", i ? "," : "", cause_name(by_count[i].second),
                       venture_side(by_count[i].second) ? " (Venture)" : "", by_count[i].first);
  out += '\n';
  if (!places.empty()) {
    std::vector<std::pair<int, std::string>> ranked;
    for (auto& [place, count] : places) ranked.emplace_back(count, place);
    std::ranges::stable_sort(ranked, std::greater{}, &std::pair<int, std::string>::first);
    out += "Main/render thread most often at:\n";
    for (std::size_t i = 0; i < ranked.size() && i < 3; ++i)
      out += std::format("  {} of {} stalls: {}\n", ranked[i].first, records.size(), ranked[i].second);
  }
  for (std::size_t i = 0; i < worst.size() && i < kShown; ++i) {
    const auto& r = *worst[i];
    out += std::format("#{} at {:.1f} s{}: {} ms{}, cause {}", r.id, r.at_s,
                       r.clock.empty() ? std::string() : " (" + r.clock + ")", r.ms,
                       r.ongoing ? " and still frozen when the log ended" : "", cause_name(r.cause));
    if (r.cpu_percent >= 0) out += std::format("; CPU {:.0f}%", r.cpu_percent);
    if (r.faults >= 0) out += std::format(", {} page faults", r.faults);
    if (r.hard_faults >= 0) out += std::format(" ({} from disk)", r.hard_faults);
    if (r.read_kib >= 0) out += std::format(", read {} KiB", r.read_kib);
    if (r.gc_ms >= 0) out += std::format(", Java GC {} ms", r.gc_ms);
    if (r.private_mib >= 0) out += std::format(", private {} MiB", r.private_mib);
    out += std::format(", {} threads", r.threads);
    if (r.ended == "unfocused") out += "; it ended when the game lost focus or quit";
    out += '\n';
    if (!r.summary.empty()) out += "  Summary: " + r.summary + "\n";
    if (i < kWithEvidence)
      for (std::size_t e = 0; e < r.evidence.size() && e < kEvidenceLines; ++e)
        out += std::format("  [{}] {}\n", r.evidence[e].kind, r.evidence[e].text);
    for (const auto& t : r.shown) {
      out += std::format("  {}{} tid {}: {} [{}{}]\n", t.name.empty() ? "(unnamed)" : t.name,
                         t.role.empty() ? std::string() : " (" + t.role + ")", t.tid, t.where, cause_name(t.cause),
                         t.moved == 0 ? ", did not move" : t.moved == 1 ? ", moving" : "");
      if (!t.stack.empty()) out += "    < " + t.stack + "\n";
    }
  }
  return out + calls_section(calls);
}

namespace {
const Record* longest_freeze(const std::vector<Record>& records, int& count) {
  const Record* longest = nullptr;
  count = 0;
  for (const auto& r : records) {
    if (r.ms < freeze_ms) continue;
    ++count;
    if (!longest || r.ms > longest->ms) longest = &r;
  }
  return longest;
}
std::string freeze_text(const Record& r, int count) {
  std::string out = std::format("{} time{}, the longest for {}{}", count, count == 1 ? "" : "s", seconds(r.ms),
                                r.clock.empty() ? std::format(" at {:.0f} s into the run", r.at_s) : " at " + r.clock);
  if (r.ongoing) out += " (still frozen when the log ended)";
  if (!r.summary.empty()) out += ": " + r.summary;
  return out;
}
}  // namespace

std::optional<Verdict> freeze_cause(const std::vector<Record>& records) {
  int count = 0;
  const auto* longest = longest_freeze(records, count);
  if (!longest) return {};
  Verdict v;
  v.cause = std::format("The game froze while it had focus {} (see Stalls).", freeze_text(*longest, count));
  v.advice = "Send this report with an issue: its Stalls section shows where the game's threads were when it froze.";
  return v;
}

std::string freeze_note(const std::vector<Record>& records) {
  int count = 0;
  const auto* longest = longest_freeze(records, count);
  return longest ? std::format("The game also froze while it had focus {} (see Stalls).",
                               freeze_text(*longest, count))
                 : std::string();
}

std::optional<Verdict> venture_cause(const std::vector<Record>& records) {
  if (records.size() < 2) return {};
  std::map<Cause, int> causes;
  std::int64_t worst = 0;
  for (const auto& r : records) {
    ++causes[r.cause];
    worst = std::max(worst, r.ms);
  }
  const auto top = std::ranges::max_element(causes, {}, &std::pair<const Cause, int>::second);
  // Clear evidence only: most stalls, on Venture's side.
  if (!venture_side(top->first) || top->second * 5 < int(records.size()) * 3) return {};
  std::string what;
  switch (top->first) {
    case Cause::jit: what = "Venture's JIT translating the game's code"; break;
    case Cause::pipeline: what = "Venture compiling shaders and pipelines"; break;
    case Cause::gc: what = "the Java runtime collecting garbage"; break;
    case Cause::paging: what = "the PC reading memory back from disk"; break;
    default: what = "Venture's own implementation of a game API"; break;
  }
  Verdict v;
  v.cause = std::format("The game stopped drawing {} times for up to {} while it had focus; {} of them were in {} "
                        "(see Stalls).", records.size(), seconds(worst), top->second, what);
  v.advice = top->first == Cause::paging
      ? "Close other programs to free memory, then play again; if the stalls stay, send this report with an issue."
      : "Send this report with an issue: the stalls point at Venture.";
  return v;
}
}  // namespace venture::stall
