#include "core/dev_game_control.h"
#include "common/environment.h"
#include "common/log.h"
#include "common/paths.h"
#include "core/process.h"
#include "core/thread_api.h"
#include "core/thread_runtime.h"
#include <chrono>
#include <atomic>
#include <cmath>
#include <fstream>
#include <set>
#include <optional>
#include <map>
#include <sstream>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace venture::dev_control {
namespace {
bool gate() {
#ifdef VENTURE_DEVELOPER_CONTROL
  const auto* run = environment::getenv("VENTURE_DEV_RUN");
  const auto* file = environment::getenv("VENTURE_DEV_GAME_CONTROL");
  return run && std::string_view(run) == "1" && file && *file;
#else
  return false;
#endif
}
using Clock = std::chrono::steady_clock;
}
std::atomic<bool> enabled{gate()};
Json parse_commands(std::string_view text) {
  if (text.size() > 1024 * 1024) throw std::invalid_argument("command file exceeds 1 MiB");
  auto root = Json::parse(text);
  if (!root.is_object() || root.value("version", 0) != 1 || !root.contains("commands") || !root["commands"].is_array())
    throw std::invalid_argument("expected version 1 and commands array");
  if (root["commands"].size() > 4096) throw std::invalid_argument("too many commands");
  std::set<std::string> ids;
  for (const auto& c : root["commands"]) {
    if (!c.is_object() || !c.contains("id") || !c["id"].is_string() || c["id"].get<std::string>().empty() ||
        !c.contains("op") || !c["op"].is_string() || !ids.insert(c["id"].get<std::string>()).second)
      throw std::invalid_argument("commands require unique nonempty string id and string op");
    auto at = c.value("after_first_frame_s", 0.0);
    if (!std::isfinite(at) || at < 0) throw std::invalid_argument("invalid after_first_frame_s");
    if (c.contains("args") && (!c["args"].is_array() || c["args"].size() > 32))
      throw std::invalid_argument("args must be an array of at most 32 values");
  }
  return root;
}
void boundary(GuestThread& thread, std::string_view function, bool completed) {
  if (!enabled) return;
  // Never call guest code while an HLE XR lock is held, or recursively while
  // the command's managed method calls back into XR.
  static thread_local bool busy = false;
  if (busy) return;
  if (completed) {
    if (function != "xrEndFrame" && function != "vrapi_SubmitFrame2") return;
  } else if (function != "xrSyncActions" && function != "vrapi_GetPredictedDisplayTime") return;
  static std::atomic<std::int64_t> first_frame_ns{-1};
  const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
  if (completed && (function == "xrEndFrame" || function == "vrapi_SubmitFrame2")) {
    if (thread.cpu().x(0) == 0) { std::int64_t expected = -1; first_frame_ns.compare_exchange_strong(expected, now_ns); }
    return;
  }
  if (completed) return;
  auto first_ns = first_frame_ns.load();
  if (first_ns < 0) return;
  std::string name;
  if (threads::describe(thread.pthread_self, nullptr, nullptr, &name) || name != "UnityMain") return;
  struct State {
    Clock::time_point next_poll{};
    std::set<std::string> done;
    std::map<std::string, Json> results;
    std::unique_ptr<Il2Cpp> api;
    std::string last_error;
    std::string failed;
    std::filesystem::path input = paths::from_utf8(environment::getenv("VENTURE_DEV_GAME_CONTROL"));
    std::filesystem::path output = [] {
      const auto* run = environment::getenv("VENTURE_DIAGNOSTIC_RUN_DIR");
      return run && *run ? paths::from_utf8(run) / "game-control-results.jsonl" :
          paths::from_utf8(environment::getenv("VENTURE_DEV_GAME_CONTROL")).parent_path() / "game-control-results.jsonl";
    }();
  };
  static auto* state = new State;
  if (Clock::now() < state->next_poll) return;
  // Only the engine main thread may invoke Unity APIs. Engine thread names
  // are a shared contract, never a package-specific selection or tid pin.
  state->next_poll = Clock::now() + std::chrono::milliseconds(250);
  struct Guard { Guard() { busy = true; } ~Guard() { busy = false; } } guard;
  auto emit = [&](Json row) {
    row["guest_tid"] = thread.tid(); row["thread"] = name;
#ifdef _WIN32
    row["host_pid"] = ::_getpid();
#else
    row["host_pid"] = ::getpid();
#endif
    row["after_first_frame_s"] = (std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count() - first_ns) / 1e9;
    std::ofstream file(state->output, std::ios::app);
    file << row.dump() << '\n'; file.flush();
    if (!file) log::error("dev-control", "Cannot write result log");
  };
  try {
    std::ifstream file(state->input, std::ios::binary);
    if (!file) return;
    std::string text;
    text.resize(1024 * 1024 + 1);
    file.read(text.data(), static_cast<std::streamsize>(text.size())); text.resize(file.gcount());
    file.close(); // Allow atomic replacement while a managed call is running.
    auto root = parse_commands(text);
    if (!state->api) state->api = std::make_unique<Il2Cpp>(thread);
    for (const auto& command : root["commands"]) {
      auto id = command["id"].get<std::string>();
      if (state->failed == id) break;
      if (state->done.contains(id)) continue;
      if ((now_ns - first_ns) / 1e9 < command.value("after_first_frame_s", 0.0)) break;
      // Commands are ordered. Wait predicates are retried without blocking the
      // Unity main thread; later commands cannot pass an unsatisfied wait.
      try {
        // A later command can use a prior returned singleton/object handle.
        // Exact result references are resolved before any guest invocation.
        auto resolve = [&](auto&& self, const Json& value) -> Json {
          if (value.is_object() && value.contains("result") && value["result"].is_string() &&
              value.size() == (value.contains("path") ? 2u : 1u)) {
            auto it = state->results.find(value["result"].get<std::string>());
            if (it == state->results.end()) throw std::runtime_error("result reference is not ready");
            return value.contains("path") ? it->second.at(Json::json_pointer(value["path"].get<std::string>())) : it->second;
          }
          if (value.is_array()) { auto out = Json::array(); for (const auto& v : value) out.push_back(self(self, v)); return out; }
          if (value.is_object()) { auto out = Json::object(); for (auto it = value.begin(); it != value.end(); ++it) out[it.key()] = self(self, it.value()); return out; }
          return value;
        };
        auto result = state->api->execute(resolve(resolve, command));
        if (command.value("op", "") == "wait" && !result.value("ready", false)) break;
        state->done.insert(id);
        state->results[id] = result;
        emit({{"id", id}, {"ok", true}, {"result", result}});
      } catch (const std::runtime_error& e) {
        state->failed = id; emit({{"id", id}, {"ok", false}, {"error", e.what()}});
        break;
      }
      break; // One operation per frame boundary, bounded introspection work.
    }
    if (root.value("sealed", false)) {
      bool complete = true;
      for (const auto& command : root["commands"]) complete &= state->done.contains(command["id"].get<std::string>());
      if (complete) { emit({{"batch_complete", true}}); enabled.store(false, std::memory_order_relaxed); }
    }
  } catch (const ProcessExit&) { throw; }
    catch (const ThreadExit&) { throw; }
    catch (const std::exception& e) {
    if (state->last_error != e.what()) { state->last_error = e.what(); emit({{"ok", false}, {"error", e.what()}}); }
  }
}
} // namespace venture::dev_control
