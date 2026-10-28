#include "hle/ovrplatform/session.h"
#include "hle/ovrplatform/social.h"

#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <random>
#include <unordered_map>

#include "common/log.h"
#include "core/guest.h"
#include "common/startup_progress.h"
#include "common/process.h"
#include "hle/ovrplatform/identity_iap.h"
#include "common/milestone.h"

namespace venture::ovrplatform {
namespace {
constexpr RequestSpec requests[] = {
#define VENTURE_REQUEST(name, type) {name, type},
#include "hle/ovrplatform/requests.inc"
#undef VENTURE_REQUEST
};
enum class Kind { message, error, init, options, model };
struct Field {
  GetterType type;
  std::uint64_t bits;
  std::size_t count = 0;
};
struct Node {
  Kind kind;
  std::uint64_t a = 0, b = 0, c = 0, d = 0, e = 0;
  std::string option_kind;
  std::map<std::string, std::vector<std::string>, std::less<>> options;
  std::map<std::string, Field, std::less<>> fields;
  std::vector<Handle> elements;
  std::string next_url, previous_url;
};
struct Graph {
  Memory& memory;
  std::vector<Handle> allocations;
  std::map<Handle, Node> nodes;
  explicit Graph(Memory& m) : memory(m) {}
  ~Graph() {
    for (auto address : allocations) memory.free(address);
  }
  Handle allocate(std::size_t size) {
    auto address = memory.allocate(size);
    if (!address) throw std::bad_alloc();
    try {
      allocations.push_back(address);
    } catch (...) {
      memory.free(address);
      throw;
    }
    return address;
  }
  Handle string(std::string_view value) {
    auto address = allocate(value.size() + 1);
    std::memcpy(g2h(address), value.data(), value.size());
    g2h<char>(address)[value.size()] = '\0';
    return address;
  }
  Handle node(Node value, std::uint64_t serial) {
    auto address = allocate(sizeof(HandleCell));
    *g2h<HandleCell>(address) = {serial};
    nodes.emplace(address, std::move(value));
    return address;
  }
  Handle model(const Model& value, std::uint64_t& serial, unsigned depth = 0) {
    if (depth > 64 || value.kind.empty())
      throw std::invalid_argument("Invalid Platform model tree");
    Node n{Kind::model};
    n.option_kind = value.kind;
    n.a = string("");
    for (const auto& [key, field] : value.fields) {
      Field out{GetterType::scalar, 0};
      std::visit(
          [&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::string>) {
              out = {GetterType::string, string(v)};
              if (key == "NextUrl") n.next_url = v;
              if (key == "PreviousUrl") n.previous_url = v;
            } else if constexpr (std::is_same_v<T, Blob>) {
              out = {GetterType::blob, allocate(v.size() + 1)};
              if (!v.empty()) std::memcpy(g2h(out.bits), v.data(), v.size());
              g2h<char>(out.bits)[v.size()] = '\0';
            } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
              // Internal graph table of guest string addresses, not an SDK wire
              // struct. Indexed getters return one borrowed string at a time.
              out = {GetterType::string_array, allocate(v.size() * sizeof(Handle) + 1), v.size()};
              for (std::size_t i = 0; i < v.size(); ++i) g2h<Handle>(out.bits)[i] = string(v[i]);
            } else if constexpr (std::is_same_v<T, std::vector<std::uint64_t>>) {
              // OVR_Types.h ovrID is 64 bits on both guest ABIs.
              out = {GetterType::id_array, allocate(v.size() * sizeof(Id) + 1), v.size()};
              if (!v.empty()) std::memcpy(g2h(out.bits), v.data(), v.size() * sizeof(Id));
            } else if constexpr (std::is_same_v<T, std::shared_ptr<const Model>>) {
              out = {GetterType::model, v ? model(*v, serial, depth + 1) : 0};
            } else if constexpr (std::is_same_v<T, float>) {
              out = {GetterType::f32, std::bit_cast<std::uint32_t>(v)};
            } else if constexpr (std::is_same_v<T, double>) {
              out = {GetterType::f64, std::bit_cast<std::uint64_t>(v)};
            } else
              out.bits = static_cast<std::uint64_t>(v);
          },
          field);
      n.fields.emplace(key, out);
    }
    if (value.kind == "DataStore")
      for (const auto& [key, field] : value.fields) n.elements.push_back(string(key));
    for (const auto& element : value.elements)
      n.elements.push_back(model(element, serial, depth + 1));
    return node(std::move(n), serial++);
  }
};
Result entitlement_handler(Provider& p, const RequestArgs&, std::stop_token stop) {
  return p.entitlement(stop);
}
Result user_handler(Provider& p, const RequestArgs&, std::stop_token stop) {
  auto r = p.logged_in_user(stop);
  if (!r.error && !r.user.id) r.error = failure(ErrorCode::invalid_response);
  if (!r.error && r.model.kind.empty())
    r.model = user_model(r.user.id, r.user.alias, r.user.display_name);
  return r;
}
Result token_handler(Provider& p, const RequestArgs&, std::stop_token stop) {
  auto r = p.access_token(stop);
  if (!r.error && r.text.empty()) r.error = failure(ErrorCode::invalid_response);
  if (!r.error && r.model.kind.empty()) r.model = {"String", {{"Value", r.text}}};
  return r;
}
Result proof_handler(Provider& p, const RequestArgs&, std::stop_token stop) {
  auto r = p.user_proof(stop);
  if (!r.error && r.text.empty()) r.error = failure(ErrorCode::invalid_response);
  if (!r.error && r.model.kind.empty()) r.model = {"UserProof", {{"Nonce", r.text}}};
  return r;
}
InitResult init_error(const Error& error) {
  if (!error) return InitResult::success;
  if (error.code == ErrorCode::unauthenticated) return InitResult::invalid_credentials;
  if (error.code == ErrorCode::not_entitled) return InitResult::not_entitled;
  return InitResult::unknown;
}
}  // namespace
std::span<const RequestSpec> request_specs() { return requests; }
std::string_view message_type_name(std::uint32_t type) {
  for (const auto& spec : requests)
    if (spec.type == type) return std::string_view(spec.name).substr(4);
  if (type == 0x1ad307b4) return "PlatformInitializeAndroidAsynchronous";
  if (type == 0x6da7ba8f) return "PlatformInitializeWindowsAsynchronous";
  if (type == 0x35692f2b) return "PlatformInitializeWithAccessToken";
  if (type == 0x51f8ce0c) return "Platform_InitializeStandaloneOculus";
  if (type == 0x2fdd0ccd) return "Notification_AssetFile_DownloadUpdate";
  return "Unknown";
}

struct Session::Impl {
  using Clock = std::chrono::steady_clock;
  enum class State { uninitialized, initializing, ready, failed, shutdown };
  struct Completion {
    Request id;
    std::uint32_t type;
    Result result;
    bool init = false;
    Clock::time_point ready_at = Clock::now();
    std::uint64_t polls_at_ready = 0;
    bool host = false;
  };
  struct Job {
    Request id;
    std::uint64_t generation;
    std::uint32_t type;
    std::string name;
    bool init = false;
    std::string app_id;
    Clock::time_point requested_at = Clock::now();
    RequestArgs args;
    HandlerSpec handler;
    bool host = false;
  };
  struct Pending {
    std::uint32_t type;
    bool init;
    bool host = false;
  };
  AppIdentity app;
  std::shared_ptr<Provider> provider;
  Memory memory;
  mutable std::mutex mutex;
  mutable std::mutex startup_mutex;
  std::map<Request, StartupRequestState> startup_requests;
  std::size_t startup_dropped = 0;
  void startup_issue(const Job& job) {
    if (!startup_diagnostics::enabled) return;
    std::lock_guard lock(startup_mutex);
    if (startup_requests.size() == 4096) { ++startup_dropped; return; }
    startup_requests[job.id] = {job.id, job.type, job.host};
  }
  void startup_received(const Job& job, int error) {
    if (!startup_diagnostics::enabled) return;
    std::lock_guard lock(startup_mutex);
    if (auto at = startup_requests.find(job.id); at != startup_requests.end()) {
      at->second.received = true; at->second.error = error;
    }
  }
  void startup_message(Request id, bool popped, bool host = false) {
    if (!startup_diagnostics::enabled) return;
    std::lock_guard lock(startup_mutex);
    if (auto at = startup_requests.find(id); at != startup_requests.end()) {
      at->second.queued = !popped; at->second.popped = popped && !host;
      at->second.taken_by_host = popped && host;
    }
  }
  std::condition_variable wake;
  State state = State::uninitialized;
  Request next_request = 1;
  Request initializing_request = 0;
  std::uint64_t generation = 1, serial = 1;
  Id user_id = 0;
  // Generation whose synchronous ovr_GetLoggedInUserID already asked the provider.
  std::uint64_t identity_attempt = 0;
  // OVR_Functions_Voip.h: app microphone mute defaults to Unmuted. This is
  // local policy only; no capture, peer, PCM ring or transport is created.
  VoipMuteState microphone_mute = VoipMuteState::unmuted;
  std::deque<Job> jobs;
  std::deque<std::pair<Job, std::function<Result(std::stop_token)>>> waits;
  std::map<Request, Pending> pending;
  std::map<std::string, HandlerSpec, std::less<>> handlers;
  std::deque<Completion> completed;
  std::map<Handle, std::unique_ptr<Graph>> graphs;
  std::unordered_map<Handle, Node*> nodes;
  Handle launch = 0;
  Clock::time_point last_poll = Clock::now();
  std::uint64_t polls = 0;
  Handle locale = 0;
  std::map<std::string, Handle, std::less<>> borrowed_strings;
  LaunchDetails launch_info;
  std::string session_key;
  nlohmann::json presence = nlohmann::json::object();
  std::deque<std::string> heartbeats;
  Clock::time_point heartbeat_due = Clock::time_point::max();
  std::jthread presence_worker;
  std::vector<std::jthread> workers;

  Impl(AppIdentity a, std::shared_ptr<Provider> p, Memory m)
      : app(std::move(a)), provider(std::move(p)), memory(std::move(m)) {
    handlers.emplace("ovr_Entitlement_GetIsViewerEntitled", HandlerSpec{entitlement_handler, ""});
    handlers.emplace("ovr_User_GetLoggedInUser", HandlerSpec{user_handler, "User"});
    handlers.emplace("ovr_User_GetAccessToken", HandlerSpec{token_handler, "String"});
    handlers.emplace("ovr_User_GetUserProof", HandlerSpec{proof_handler, "UserProof"});
    std::random_device random;
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; ++i) session_key += hex[random() & 15];
    for (const auto& spec : identity_iap_services())
      handlers.emplace(spec.name, HandlerSpec{
          [name = spec.name](Provider& p, const RequestArgs& args, std::stop_token stop) {
            return p.service(name, args, stop);
          }, std::string(spec.payload)});
    try {
      presence_worker = std::jthread([this](std::stop_token stop) { run_presence(stop); });
      for (unsigned i = 0; i < 3; ++i)
        workers.emplace_back([this](std::stop_token stop) { run(stop); });
      workers.emplace_back(
          [this](std::stop_token stop) { wait_results(stop); });
    } catch (...) {
      for (auto& worker : workers) worker.request_stop();
      presence_worker.request_stop();
      wake.notify_all();
      workers.clear();
      throw;
    }
  }
  ~Impl() {
    presence_worker.request_stop();
    for (auto& worker : workers) worker.request_stop();
    wake.notify_all();
    for (auto& worker : workers)
      if (worker.joinable()) worker.join();
  }
  static bool local(std::string_view name) {
    return name == "ovr_GroupPresence_Clear" || name == "ovr_GroupPresence_Set" ||
        name == "ovr_GroupPresence_SetDestination" || name == "ovr_GroupPresence_SetLobbySession" ||
        name == "ovr_GroupPresence_SetMatchSession" || name == "ovr_GroupPresence_SetIsJoinable" ||
        name == "ovr_GroupPresence_SetDeeplinkMessageOverride" || name == "ovr_AbuseReport_ReportRequestHandled" ||
        name == "ovr_Application_GetVersion" || name == "ovr_ApplicationLifecycle_GetSessionKey" ||
        name == "ovr_ApplicationLifecycle_RegisterSessionKey" || name == "ovr_ApplicationLifecycle_GetRegisteredPIDs";
  }
  Result local_request(const Job& job) { // caller holds mutex; never does I/O
    const auto& name = job.name;
    const auto& args = job.args.values;
    if (name.starts_with("ovr_GroupPresence_")) {
      auto next = presence;
      if (name == "ovr_GroupPresence_Clear") next = nlohmann::json::object();
      else {
        // RichPresenceService::Set (RVA 0xf8410) stores these JSON properties;
        // set_* / launch_* at 0xf7f40 belong to local analytics, not the wire.
        if (next.empty() || name == "ovr_GroupPresence_Set")
          next = {{"api_name", ""}, {"lobby_session_id", ""}, {"match_session_id", ""},
                      {"deeplink_message_override", ""}, {"is_joinable", false}};
        if (name == "ovr_GroupPresence_Set") {
          const auto& options = std::get<OptionsSnapshot>(args.at(0));
          for (auto [field, key] : {std::pair{"DestinationApiName", "api_name"},
               {"LobbySessionId", "lobby_session_id"}, {"MatchSessionId", "match_session_id"},
               {"DeeplinkMessageOverride", "deeplink_message_override"}})
            if (auto it = options.fields.find(field); it != options.fields.end() && !it->second.empty())
              next[key] = it->second.front();
          if (auto it = options.fields.find("IsJoinable"); it != options.fields.end() && !it->second.empty())
            next["is_joinable"] = it->second.front() == "1";
        } else if (name == "ovr_GroupPresence_SetIsJoinable")
          next["is_joinable"] = std::get<bool>(args.at(0));
        else {
          const char* key = name == "ovr_GroupPresence_SetDestination" ? "api_name" :
              name == "ovr_GroupPresence_SetLobbySession" ? "lobby_session_id" :
              name == "ovr_GroupPresence_SetMatchSession" ? "match_session_id" : "deeplink_message_override";
          next[key] = std::get<std::string>(args.at(0));
        }
      }
      for (const auto& field : next)
        if (field.is_string() && field.get_ref<const std::string&>().size() > 4096)
          return {failure(ErrorCode::invalid_argument)};
      if (next == presence) return {}; // Meta 0x1876a0 also suppresses unchanged state.
      if (heartbeats.size() >= 256) return {failure(ErrorCode::unavailable)};
      // RichPresenceClearRequest (0xf89d0) hands the heartbeat an empty string.
      heartbeats.push_back(next.empty() ? std::string{} : next.dump());
      presence = std::move(next);
      heartbeat_due = Clock::time_point::max();
      wake.notify_all();
      return {};
    }
    if (name == "ovr_Application_GetVersion") {
      Result r;
      // Latest store version is unknown. Do not claim this build is the latest.
      r.model = {"ApplicationVersion", {{"CurrentCode", app.version_code}, {"CurrentName", app.version_name}}};
      return r;
    }
    if (name == "ovr_ApplicationLifecycle_GetSessionKey")
      return {{}, {}, {}, {"String", {{"Value", session_key}}}};
    if (name == "ovr_ApplicationLifecycle_RegisterSessionKey")
      return std::get<std::string>(args.at(0)) == session_key ? Result{} : Result{failure(ErrorCode::invalid_argument)};
    if (name == "ovr_ApplicationLifecycle_GetRegisteredPIDs") {
      Result r;
      r.model = {"PidArray", {}, {{"Pid", {{"Id", std::to_string(current_process_id())}}}}};
      return r;
    }
    if (name == "ovr_AbuseReport_ReportRequestHandled") return {};
    return {unsupported_operation(name)};
  }
  void run_presence(std::stop_token stop) {
    std::string current;
    for (;;) {
      {
        std::unique_lock lock(mutex);
        const auto ready = [&] { return stop.stop_requested() || !heartbeats.empty(); };
        if (heartbeat_due == Clock::time_point::max()) wake.wait(lock, ready);
        else wake.wait_until(lock, heartbeat_due, ready);
        if (stop.stop_requested()) return;
        if (!heartbeats.empty()) { current = std::move(heartbeats.front()); heartbeats.pop_front(); }
        heartbeat_due = Clock::time_point::max();
      }
      Result result;
      try { result = provider->heartbeat(current, stop); }
      catch (...) { result.error = failure(ErrorCode::unavailable); }
      std::lock_guard lock(mutex);
      if (stop.stop_requested()) return;
      if (result.error) {
        log::warn("ovrplatform", "Presence heartbeat failed error={}", static_cast<int>(result.error.code));
      } else {
        try { heartbeat_due = Clock::now() + std::chrono::seconds(std::stoi(result.text)); }
        catch (...) { log::warn("ovrplatform", "Presence heartbeat invalid interval"); }
      }
    }
  }
  Request id() {
    // Never reuse IDs, including after shutdown; 0 is SDK invalidRequestID.
    if (!next_request) throw std::overflow_error("Platform request IDs exhausted");
    return next_request++;
  }
  // The app id the game declares at initialization is its identity, as on a
  // headset: imports and development builds may not match the install plan's
  // (or have none). Meta's servers answer ownership for it; only a malformed
  // id, or another id once a session is up, fails. Called under mutex.
  Error validate(std::string_view requested) {
    if (!valid_id(requested) || (state == State::ready && requested != app.app_id))
      return failure(ErrorCode::identity_mismatch);
    if (requested != app.app_id) {
      log::info("ovrplatform", "using the app id the game declares (install plan: {})",
                app.app_id.empty() ? "none" : "a different id");
      app.app_id = requested;
      provider->declare_app_id(requested);
    }
    return {};
  }
  void finish(const Job& job, Result result) {
    startup_received(job, static_cast<int>(result.error.code));
    if (job.generation != generation || !pending.erase(job.id)) return;
    if (job.init && initializing_request == job.id) {
      state = result.error ? State::failed : State::ready;
      initializing_request = 0;
      wake.notify_all();
    }
    if (!result.error && job.name == "ovr_User_GetLoggedInUser") user_id = result.user.id;
    if (result.error)
      log::warn("ovrplatform", "{} request={} type={:#x} error={}", job.name, job.id, job.type,
                static_cast<int>(result.error.code));
    else if (job.name.starts_with("ovr_GroupPresence_Set") || job.name == "ovr_GroupPresence_Clear")
      log::info("ovrplatform", "{} request={} type={:#x} completed", job.name, job.id, job.type);
    else
      log::debug("ovrplatform", "{} request={} type={:#x} completed", job.name,
                 job.id, job.type);
    if (job.name == "ovr_User_GetUserProof") {
      milestone::mark("platform_proof_ready");
      log::info("ovrplatform", "User proof request={} ready after={}ms error={}",
                job.id, std::chrono::duration_cast<std::chrono::milliseconds>(
                    Clock::now() - job.requested_at).count(),
                static_cast<int>(result.error.code));
    }
    completed.push_back({job.id, job.type, std::move(result), job.init, Clock::now(), polls, job.host});
    startup_message(job.id, false);
  }
  void run(std::stop_token stop) {
    for (;;) {
      Job job;
      {
        std::unique_lock lock(mutex);
        wake.wait(lock, [&] {
          return stop.stop_requested() ||
                 (!jobs.empty() && (state != State::initializing || jobs.front().init));
        });
        if (stop.stop_requested()) return;
        job = std::move(jobs.front());
        jobs.pop_front();
        // Requests made during initialization must apply local state in issue
        // order, even when several request workers wake together.
        if (!job.init && local(job.name)) {
          Result result;
          try {
            result = state == State::ready && job.generation == generation
                ? local_request(job) : Result{failure(ErrorCode::uninitialized)};
          } catch (...) { result.error = failure(ErrorCode::invalid_argument); }
          finish(job, std::move(result));
          continue;
        }
      }
      Result result;
      try {
        if (job.init) {
          {
            std::lock_guard lock(mutex);
            result.error = validate(job.app_id);
          }
          if (!result.error) result.error = provider->prepare();
        } else {
          bool ready;
          {
            std::lock_guard lock(mutex);
            ready = state == State::ready && job.generation == generation;
          }
          if (!ready)
            result.error = failure(ErrorCode::uninitialized);
          else if (job.handler.handler)
            result = job.handler.handler(*provider, job.args, stop);
          else
            result.error = unsupported_operation(job.name);
        }
      } catch (...) {
        result.error = failure(ErrorCode::unavailable);
      }
      if (!result.deferred) startup_received(job, static_cast<int>(result.error.code));
      std::lock_guard lock(mutex);
      if (!result.error && result.deferred && pending.contains(job.id) &&
          job.generation == generation) {
        waits.emplace_back(std::move(job), std::move(result.deferred));
        wake.notify_all();
        continue;
      }
      if (!job.init && !local(job.name) && !result.error && result.model.kind != job.handler.payload_kind)
        result.error = failure(ErrorCode::invalid_response);
      finish(job, std::move(result));
    }
  }
  void wait_results(std::stop_token stop) {
    for (;;) {
      std::pair<Job, std::function<Result(std::stop_token)>> task;
      {
        std::unique_lock lock(mutex);
        wake.wait(lock,
                  [&] { return stop.stop_requested() || !waits.empty(); });
        if (stop.stop_requested()) return;
        task = std::move(waits.front());
        waits.pop_front();
      }
      Result result;
      try {
        result = task.second(stop);
      } catch (...) {
        result.error = failure(ErrorCode::unavailable);
      }
      if (!result.error &&
          (result.deferred ||
           result.model.kind != task.first.handler.payload_kind))
        result.error = failure(ErrorCode::invalid_response);
      startup_received(task.first, static_cast<int>(result.error.code));
      std::lock_guard lock(mutex);
      finish(task.first, std::move(result));
    }
  }
  Handle publish(std::unique_ptr<Graph> graph, Handle root) {
    std::vector<Handle> inserted;
    inserted.reserve(graph->nodes.size());
    try {
      for (auto& [handle, node] : graph->nodes) {
        nodes.emplace(handle, &node);
        inserted.push_back(handle);
      }
      graphs.emplace(root, std::move(graph));
    } catch (...) {
      for (auto handle : inserted) nodes.erase(handle);
      throw;
    }
    return root;
  }
  // Called under mutex; strings remain borrowed until session shutdown.
  Handle borrow_string(std::string_view value) {
    if (state == State::shutdown) return 0;
    if (auto it = borrowed_strings.find(value); it != borrowed_strings.end()) return it->second;
    auto graph = std::make_unique<Graph>(memory);
    auto root = graph->string(value);
    publish(std::move(graph), root);
    borrowed_strings.emplace(std::string(value), root);
    return root;
  }
  void destroy(Handle root) {
    auto it = graphs.find(root);
    if (it == graphs.end()) return;
    for (auto& [handle, node] : it->second->nodes) nodes.erase(handle);
    graphs.erase(it);
  }
  Node* find(Handle h, Kind kind) const {
    auto it = nodes.find(h);
    return it != nodes.end() && it->second->kind == kind ? it->second : nullptr;
  }
};
Session::Session(AppIdentity a, std::shared_ptr<Provider> p, Memory m)
    : impl_(std::make_unique<Impl>(std::move(a), std::move(p), std::move(m))) {
  register_social_handlers(*this);
}
Session::~Session() { shutdown(); }
StartupPlatformState Session::startup_snapshot() const {
  StartupPlatformState out;
  if (!startup_diagnostics::enabled) return out;
  {
    std::lock_guard lock(impl_->startup_mutex);
    for (const auto& [id, value] : impl_->startup_requests) out.requests.push_back(value);
    out.dropped = impl_->startup_dropped;
  }
  // A stuck platform lock must not prevent the rest of the diagnostic dump.
  std::unique_lock lock(impl_->mutex, std::try_to_lock);
  out.lock_available = lock.owns_lock();
  if (out.lock_available) {
    out.state = static_cast<int>(impl_->state); out.polls = impl_->polls;
    out.pending = impl_->pending.size(); out.queued = impl_->completed.size();
  }
  return out;
}
void Session::log_startup_snapshot() const {
  const auto s = startup_snapshot();
  log::info("startup-snapshot", "platform lock_available={} state={} pending={} queued={} polls={} trace_dropped={}",
            s.lock_available, s.state, s.pending, s.queued, s.polls, s.dropped);
  for (const auto& r : s.requests)
    log::info("startup-snapshot", "request id={} type={:#x} host={} received={} queued={} popped={} taken_by_host={} error={}",
              r.id, r.type, r.host, r.received, r.queued, r.popped, r.taken_by_host, r.error);
}
void Session::set_microphone_mute(VoipMuteState value) {
  std::lock_guard lock(impl_->mutex);
  if (value == VoipMuteState::unknown || value == VoipMuteState::muted ||
      value == VoipMuteState::unmuted)
    impl_->microphone_mute = value;
}
VoipMuteState Session::microphone_mute() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->microphone_mute;
}
InitResult Session::initialize(std::string_view app_id) {
  log::info("ovrplatform", "initialize app_id_valid={} app_id_matches={}",
            valid_id(app_id), app_id == impl_->app.app_id);
  std::uint64_t generation;
  {
    std::lock_guard lock(impl_->mutex);
    if (auto error = impl_->validate(app_id)) return init_error(error);
    if (impl_->state == Impl::State::ready) return InitResult::success;
    if (impl_->state == Impl::State::shutdown || impl_->state == Impl::State::initializing)
      return InitResult::uninitialized;
    impl_->state = Impl::State::initializing;
    generation = impl_->generation;
  }
  Error error;
  try {
    error = impl_->provider->prepare();
  } catch (...) {
    error = failure(ErrorCode::unauthenticated);
  }
  std::lock_guard lock(impl_->mutex);
  if (generation != impl_->generation) return InitResult::uninitialized;
  impl_->state = error ? Impl::State::failed : Impl::State::ready;
  impl_->wake.notify_all();
  if (error)
    log::warn("ovrplatform", "initialization failed error={}", static_cast<int>(error.code));
  return init_error(error);
}
Request Session::initialize_async(std::string_view app_id, std::uint32_t type) {
  log::info("ovrplatform", "initialize_async app_id_valid={} app_id_matches={}",
            valid_id(app_id), app_id == impl_->app.app_id);
  std::lock_guard lock(impl_->mutex);
  Impl::Job job{impl_->id(), impl_->generation,  type, "ovr_PlatformInitializeAndroidAsynchronous",
                true,        std::string(app_id)};
  impl_->pending.emplace(job.id, Impl::Pending{job.type, job.init});
  impl_->startup_issue(job);
  if (auto error = impl_->validate(app_id))
    impl_->finish(job, {std::move(error)});
  else if (impl_->state == Impl::State::shutdown)
    impl_->finish(job, {failure(ErrorCode::cancelled)});
  else if (impl_->state == Impl::State::ready)
    impl_->finish(job, {});
  else if (impl_->state == Impl::State::initializing)
    impl_->finish(job, {failure(ErrorCode::uninitialized)});
  else {
    impl_->state = Impl::State::initializing;
    impl_->initializing_request = job.id;
    impl_->jobs.push_back(std::move(job));
    impl_->wake.notify_all();
  }
  return job.id;
}
Request Session::reject_initialization(std::uint32_t type) {
  std::lock_guard lock(impl_->mutex);
  auto id = impl_->id();
  Impl::Job diagnostic{id, impl_->generation, type, "rejected initialization", true};
  impl_->startup_issue(diagnostic);
  impl_->startup_received(diagnostic, static_cast<int>(ErrorCode::unsupported));
  impl_->completed.push_back({id, type, {failure(ErrorCode::unsupported)}, true});
  impl_->startup_message(id, false);
  log::warn("ovrplatform", "unsupported initialization request={} type={:#x}", id, type);
  return id;
}
bool Session::initialized() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->state == Impl::State::ready;
}
Id Session::logged_in_user_id() const {
  // On a headset the user id is known once initialization succeeds, and games call this before any
  // request (Moss dereferences the user it then expects). Resolve the app-scoped identity on the first
  // call; later calls, including after a failure, stay local so a polling frame thread never waits again.
  std::uint64_t generation;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->state != Impl::State::ready) return 0;
    if (impl_->user_id || impl_->identity_attempt == impl_->generation) return impl_->user_id;
    generation = impl_->identity_attempt = impl_->generation;
  }
  Result result;
  try {
    result = impl_->provider->logged_in_user({});
  } catch (...) {
    result.error = failure(ErrorCode::unavailable);
  }
  std::lock_guard lock(impl_->mutex);
  if (impl_->generation != generation || impl_->state != Impl::State::ready) return 0;
  if (result.error || !result.user.id) {
    log::warn("ovrplatform", "ovr_GetLoggedInUserID failed: {}",
              static_cast<int>(result.error ? result.error.code : ErrorCode::invalid_response));
    return impl_->user_id;
  }
  return impl_->user_id = result.user.id;
}
Request Session::request(const RequestSpec& spec) { return request(spec, {}); }
Request Session::request(const RequestSpec& spec, RequestArgs args) {
  return enqueue(spec, std::move(args), false);
}
Request Session::request_host(const RequestSpec& spec, RequestArgs args) {
  return enqueue(spec, std::move(args), true);
}
Request Session::enqueue(const RequestSpec& spec, RequestArgs args, bool host) {
  std::lock_guard lock(impl_->mutex);
  Impl::Job job{impl_->id(), impl_->generation, spec.type, spec.name};
  job.args = std::move(args);
  job.host = host;
  log::info("ovrplatform", "{} request={} type={:#x}", job.name, job.id, job.type);
  impl_->pending.emplace(job.id, Impl::Pending{job.type, job.init, host});
  impl_->startup_issue(job);
  auto handler = impl_->handlers.find(job.name);
  const bool supported = handler != impl_->handlers.end() || Impl::local(job.name);
  if (handler != impl_->handlers.end()) job.handler = handler->second;
  if (impl_->state == Impl::State::shutdown)
    impl_->finish(job, {failure(ErrorCode::cancelled)});
  else if (job.args.error)
    impl_->finish(job, {job.args.error});
  else if (impl_->state != Impl::State::ready && impl_->state != Impl::State::initializing)
    impl_->finish(job, {failure(ErrorCode::uninitialized)});
  else if (!supported && impl_->state == Impl::State::ready)
    impl_->finish(job, {unsupported_operation(job.name)});
  else if (Impl::local(job.name) && impl_->state == Impl::State::ready) {
    try { impl_->finish(job, impl_->local_request(job)); }
    catch (...) { impl_->finish(job, {failure(ErrorCode::invalid_argument)}); }
  }
  else {
    impl_->jobs.push_back(std::move(job));
    impl_->wake.notify_all();
  }
  return job.id;
}
Handle Session::pop() {
  std::lock_guard lock(impl_->mutex);
  const auto now = Impl::Clock::now();
  const auto poll_gap = now - impl_->last_poll;
  impl_->last_poll = now;
  ++impl_->polls;
  auto completion = std::find_if(impl_->completed.begin(), impl_->completed.end(),
                                  [](const auto& c) { return !c.host; });
  if (completion == impl_->completed.end()) return 0;
  const auto& c = *completion;
  auto graph = std::make_unique<Graph>(impl_->memory);
  Handle error = 0, payload = 0;
  if (c.result.error) {
    error = graph->node(
        {Kind::error, static_cast<std::uint64_t>(static_cast<std::int64_t>(c.result.error.code)),
         static_cast<std::uint64_t>(c.result.error.http_status),
         graph->string(c.result.error.message)},
        impl_->serial++);
  }
  if (c.init)
    payload = graph->node({Kind::init, static_cast<std::uint64_t>(init_error(c.result.error))},
                          impl_->serial++);
  else if (!error && !c.result.model.kind.empty())
    payload = graph->model(c.result.model, impl_->serial);
  Handle string = 0;
  if (!error && c.result.model.kind == "String") {
    auto* n = &graph->nodes.at(payload);
    auto it = n->fields.find("Value");
    if (it != n->fields.end() && it->second.type == GetterType::string) string = it->second.bits;
  }
  auto root = graph->node({Kind::message, c.type, c.id, error, payload, string}, impl_->serial++);
  auto out = impl_->publish(std::move(graph), root);
  if (!error && c.type == 0x186b58b1) {
    log::info("ovrplatform", "Delivered entitlement success");
    startup_diagnostics::progress();
  }
  if (!error && c.type == 0x22810483) {
    milestone::mark("platform_proof_popped");
    log::info("ovrplatform", "Delivered user proof fields=[nonce:string:length={}] "
              "request={} queue_age={}ms poll_gap={}ms polls_since_ready={}",
              c.result.text.size(), c.id,
              std::chrono::duration_cast<std::chrono::milliseconds>(now - c.ready_at).count(),
              std::chrono::duration_cast<std::chrono::milliseconds>(poll_gap).count(),
              impl_->polls - c.polls_at_ready);
  }
  if (!error && c.type == 0x21cbe0c0) {
    if (auto it = c.result.model.fields.find("AgeCategory");
        it != c.result.model.fields.end())
      if (auto category = std::get_if<std::int32_t>(&it->second))
        log::info("ovrplatform", "Delivered user age category enum={} queue_age={}ms",
                  *category, std::chrono::duration_cast<std::chrono::milliseconds>(
                      now - c.ready_at).count());
  }
  impl_->startup_message(c.id, true);
  impl_->completed.erase(completion);
  return out;
}
std::optional<Result> Session::take_result(Request request) {
  std::lock_guard lock(impl_->mutex);
  for (auto it = impl_->completed.begin(); it != impl_->completed.end(); ++it) {
    if (!it->host || it->id != request) continue;
    auto result = std::move(it->result);
    impl_->startup_message(it->id, true, true);
    impl_->completed.erase(it);
    return result;
  }
  return std::nullopt;
}
void Session::free_message(Handle h) {
  std::lock_guard lock(impl_->mutex);
  if (impl_->find(h, Kind::message)) impl_->destroy(h);
}
std::uint64_t Session::get(Handle h, std::string_view name) const { return get(h, name, 0); }
std::uint64_t Session::get(Handle h, std::string_view name, std::uint64_t index) const {
  std::lock_guard lock(impl_->mutex);
  if (name.starts_with("ovr_Message_")) {
    auto* n = impl_->find(h, Kind::message);
    if (!n) return 0;
    if (name == "ovr_Message_GetType") return n->a;
    if (name == "ovr_Message_GetRequestID") return n->b;
    if (name == "ovr_Message_IsError") return n->c != 0;
    if (name == "ovr_Message_GetError") return n->c;
    if (name == "ovr_Message_GetString") return n->e;
    // OVR_Message.h returns an ovrMessageHandle. Unity's Message.cs unwraps
    // this before its typed getters; our graph is already the native message.
    if (name == "ovr_Message_GetNativeMessage") return h;
    if (name == "ovr_Message_GetPlatformInitialize")
      return impl_->find(n->d, Kind::init) ? n->d : 0;
    if (n->c || !name.starts_with("ovr_Message_Get")) return 0;
    auto* payload = impl_->find(n->d, Kind::model);
    return payload && payload->option_kind == name.substr(15) ? n->d : 0;
  }
  if (auto* n = impl_->find(h, Kind::error)) {
    if (name == "ovr_Error_GetCode") return n->a;
    if (name == "ovr_Error_GetHttpCode") return n->b;
    if (name == "ovr_Error_GetMessage" || name == "ovr_Error_GetDisplayableMessage") return n->c;
  }
  if (auto* n = impl_->find(h, Kind::init); n && name == "ovr_PlatformInitialize_GetResult")
    return n->a;
  auto* d = model_getter(name);
  auto* n = impl_->find(h, Kind::model);
  if (!d) return 0;
  if (!n || n->option_kind != d->model) {
    // A failed request has no model. Optional text is safely empty even when
    // a client asks through that null handle. Do not invent a model, identity
    // or successful response, and keep rejecting stale/wrong-type handles.
    return !h && d->op == GetterOp::field && d->type == GetterType::string
               ? impl_->borrow_string("") : 0;
  }
  auto typed = [&](Field field) -> std::uint64_t {
    if (field.type != d->type) return 0;
    if (field.type == GetterType::model && field.bits) {
      auto* child = impl_->find(field.bits, Kind::model);
      if (!child || child->option_kind != d->nested) return 0;
    }
    return field.bits;
  };
  switch (d->op) {
    case GetterOp::size:
      return n->elements.size();
    case GetterOp::element:
      return index < n->elements.size() ? typed({GetterType::model, n->elements[index]}) : 0;
    case GetterOp::indexed:
    case GetterOp::indexed_size: {
      auto it = n->fields.find(d->field);
      if (it == n->fields.end() || (it->second.type != GetterType::id_array &&
                                   it->second.type != GetterType::string_array)) return 0;
      const auto& field = it->second;
      if (d->op == GetterOp::indexed_size) return field.count;
      if ((field.type == GetterType::string_array) != (d->type == GetterType::string)) return 0;
      // OVR_MatchmakingEnqueuedUser.h / CAPI.cs and Meta 1.205 ContentRating
      // getters (0x7c520/0x7c5a0, mov ecx,edx) use uint32 indices, unlike
      // Array_GetElement's guest size_t. AAPCS64 leaves upper bits unspecified.
      const auto element = static_cast<std::uint32_t>(index);
      return element < field.count ? g2h<const Id>(field.bits)[element] : 0;
    }
    case GetterOp::has_next:
      return !n->next_url.empty();
    case GetterOp::has_previous:
      return !n->previous_url.empty();
    case GetterOp::dict_size:
      return n->fields.size();
    case GetterOp::dict_key: {
      // OVR_DataStore.h: int index, 32 bits on both guest ABIs.
      const auto element = static_cast<std::uint32_t>(index);
      return element < n->elements.size() ? n->elements[element] : 0;
    }
    case GetterOp::dict_contains:
    case GetterOp::dict_value: {
      if (!index) return 0;
      auto it = n->fields.find(g2h<const char>(index));
      if (d->op == GetterOp::dict_contains) return it != n->fields.end();
      return it == n->fields.end() ? 0 : typed(it->second);
    }
    case GetterOp::field: {
      auto it = n->fields.find(d->field);
      return it == n->fields.end() ? (d->type == GetterType::string ? n->a : 0) : typed(it->second);
    }
  }
  return 0;
}
Handle Session::launch_details() {
  std::lock_guard lock(impl_->mutex);
  if (impl_->state == Impl::State::shutdown) return 0;
  if (impl_->launch) return impl_->launch;
  auto graph = std::make_unique<Graph>(impl_->memory);
  const auto& info = impl_->launch_info;
  Model launch{"LaunchDetails",
               {{"LaunchType", info.type},
                {"RoomID", info.room},
                {"LaunchSource", info.source},
                {"DeeplinkMessage", info.deeplink}, {"DestinationApiName", info.destination},
                {"LobbySessionID", info.lobby}, {"MatchSessionID", info.match},
                {"Users", std::make_shared<const Model>(Model{"UserArray"})}}};
  auto root = graph->model(launch, impl_->serial);
  impl_->launch = impl_->publish(std::move(graph), root);
  return root;
}
void Session::set_launch_details(const LaunchDetails& details) {
  const auto checked = read_launch_details(launch_details_json(details));
  std::lock_guard lock(impl_->mutex);
  if (impl_->launch || impl_->state == Impl::State::shutdown)
    throw std::logic_error("Launch details already borrowed or session closed");
  impl_->launch_info = checked;
}
void Session::attach_launcher(std::shared_ptr<PipeChannel> channel) {
  if (!channel) throw std::invalid_argument("Missing launcher channel");
  register_handler("ovr_Application_LaunchOtherApp", {[channel = std::move(channel)]
      (Provider&, const RequestArgs& args, std::stop_token stop) -> Result {
    try {
      LaunchDetails details;
      details.type = 4; details.source = "application";
      const auto& options = std::get<OptionsSnapshot>(args.values.at(1));
      for (auto [field, out] : {std::pair{"DeeplinkMessage", &details.deeplink},
           {"DestinationApiName", &details.destination}, {"LobbySessionId", &details.lobby},
           {"MatchSessionId", &details.match}})
        if (auto it = options.fields.find(field); it != options.fields.end() && !it->second.empty()) *out = it->second.front();
      if (auto it = options.fields.find("RoomId"); it != options.fields.end() && !it->second.empty())
        details.room = std::stoull(it->second.front());
      if (stop.stop_requested()) return {failure(ErrorCode::cancelled)};
      auto reply = channel->call({{"op", "launch_other_app"},
          {"app", std::to_string(std::get<Id>(args.values.at(0)))}, {"details", launch_details_json(details)}});
      if (!reply.value("success", false)) return {unsupported_operation("ovr_Application_LaunchOtherApp")};
      return {{}, {}, {}, {"String", {{"Value", std::string{}}}}};
    } catch (...) { return {stop.stop_requested() ? failure(ErrorCode::cancelled) : unsupported_operation("ovr_Application_LaunchOtherApp")}; }
  }, "String"});
}
Handle Session::locale() {
  std::lock_guard lock(impl_->mutex);
  if (impl_->state == Impl::State::shutdown) return 0;
  if (impl_->locale) return impl_->locale;
  auto graph = std::make_unique<Graph>(impl_->memory);
  auto root = graph->string(host_locale());
  impl_->locale = impl_->publish(std::move(graph), root);
  return root;
}
Handle Session::message_type_string(std::uint32_t type) {
  return enum_string(message_type_name(type));
}
Handle Session::enum_string(std::string_view name) {
  std::lock_guard lock(impl_->mutex);
  return impl_->borrow_string(name);
}
Handle Session::create_options(std::string_view kind) {
  std::lock_guard lock(impl_->mutex);
  if (impl_->state == Impl::State::shutdown) return 0;
  auto graph = std::make_unique<Graph>(impl_->memory);
  Node node{Kind::options};
  node.option_kind = kind;
  auto root = graph->node(std::move(node), impl_->serial++);
  return impl_->publish(std::move(graph), root);
}
void Session::destroy_options(Handle h, std::string_view kind) {
  std::lock_guard lock(impl_->mutex);
  if (auto* n = impl_->find(h, Kind::options); n && n->option_kind == kind) impl_->destroy(h);
}
void Session::set_option(Handle h, std::string_view kind, std::string_view field,
                         std::vector<std::string> values, bool append) {
  std::lock_guard lock(impl_->mutex);
  if (auto* n = impl_->find(h, Kind::options); n && n->option_kind == kind) {
    std::string property(field);
    // AddSuggestedUser/ClearSuggestedUsers etc. name the same collection.
    if (append) property += 's';
    // Dictionary setters carry (key, value); preserve other keys and replace
    // a single key even when its scalar type changes. NUL separates host keys.
    if (values.size() == 2) {
      for (std::string_view type : {"String", "Int", "Double"}) {
        if (!property.ends_with(type)) continue;
        property.resize(property.size() - type.size());
        property += '\0';
        property += values[0];
        values[0] = type;
        break;
      }
    }
    auto& existing = n->options[property];
    if (!append) existing.clear();
    existing.insert(existing.end(), std::make_move_iterator(values.begin()),
                    std::make_move_iterator(values.end()));
  } else
    log::warn("ovrplatform", "invalid options handle for {}", kind);
}
void Session::clear_options(Handle h, std::string_view kind, std::string_view field) {
  std::lock_guard lock(impl_->mutex);
  if (auto* n = impl_->find(h, Kind::options); n && n->option_kind == kind) {
    auto prefix = std::string(field) + '\0';
    std::erase_if(n->options, [&](const auto& entry) {
      return entry.first == field || entry.first.starts_with(prefix);
    });
  }
}
std::optional<OptionsSnapshot> Session::snapshot_options(Handle h, std::string_view kind) const {
  std::lock_guard lock(impl_->mutex);
  auto* n = impl_->find(h, Kind::options);
  if (!n || n->option_kind != kind) return {};
  return OptionsSnapshot{n->option_kind, n->options};
}
std::optional<PageSnapshot> Session::snapshot_page(Handle h, std::string_view kind) const {
  std::lock_guard lock(impl_->mutex);
  auto* n = impl_->find(h, Kind::model);
  if (!n || n->option_kind != kind) return {};
  return PageSnapshot{n->option_kind, n->next_url, n->previous_url};
}
void Session::register_handler(std::string_view name, HandlerSpec handler) {
  if (!handler.handler) throw std::invalid_argument("Platform handler is empty");
  std::lock_guard lock(impl_->mutex);
  impl_->handlers.insert_or_assign(std::string(name), std::move(handler));
}
void Session::notify(std::uint32_t type, Model model) {
  std::lock_guard lock(impl_->mutex);
  if (impl_->state != Impl::State::ready) return;
  Result result;
  result.model = std::move(model);
  impl_->completed.push_back({0, type, std::move(result)});
}
void Session::shutdown() {
  std::lock_guard lock(impl_->mutex);
  if (impl_->state == Impl::State::shutdown) return;
  impl_->state = Impl::State::shutdown;
  ++impl_->generation;
  impl_->user_id = 0;
  impl_->microphone_mute = VoipMuteState::unmuted;
  for (auto& completion : impl_->completed) completion.result = {failure(ErrorCode::cancelled)};
  for (const auto& [id, job] : impl_->pending)
    impl_->completed.push_back({id, job.type, {failure(ErrorCode::cancelled)}, job.init,
                                Impl::Clock::now(), 0, job.host});
  impl_->pending.clear();
  impl_->jobs.clear();
  impl_->waits.clear();
  impl_->nodes.clear();
  impl_->graphs.clear();
  impl_->launch = 0;
  impl_->locale = 0;
  impl_->borrowed_strings.clear();
  impl_->heartbeats.clear();
  impl_->presence_worker.request_stop();
  for (auto& worker : impl_->workers) worker.request_stop();
  impl_->wake.notify_all();
}
}  // namespace venture::ovrplatform
