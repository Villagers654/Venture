#include "hle/steam/bridge.h"
#include "platform/steam_client.h"
#include "common/log.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace venture::steam {
Bridge& Bridge::get() { static auto* bridge = new Bridge; return *bridge; }
const Sdk& Bridge::sdk() const {
  if (!sdk_) throw std::runtime_error("Steam bridge has no verified guest SDK");
  return *sdk_;
}
void Bridge::configure(Plan plan, const Sdk& sdk) {
  std::lock_guard lock(mutex);
  if (sdk_ || initialized_) throw std::runtime_error("Steam launch identity is immutable");
  if (!plan.appid) throw std::runtime_error("Steam app ID absent from plan");
  platform::prepare_steam_environment(plan.appid);
  plan_ = std::move(plan);
  sdk_ = &sdk;
}
void* Bridge::host_interface(const Interface& type) {
  if (type.name == "ISteamClient") return client_;
  const auto direct = "SteamAPI_ISteamClient_Get" + std::string(type.name);
  const auto version = std::string(type.version);
  void* result{};
  if (type.name == "ISteamUtils") {
    result = member<void*>(client_, sdk().slot(direct), pipe_, version.c_str());
  } else {
    auto it = std::ranges::find(sdk().methods, direct, &Method::symbol);
    auto slot = it == sdk().methods.end() ? sdk().slot("SteamAPI_ISteamClient_GetISteamGenericInterface") : it->slot;
    result = member<void*>(client_, slot, user_, pipe_, version.c_str());
  }
  if (!result) throw std::runtime_error("Running Steam client lacks interface " + version);
  return result;
}
int Bridge::initialize(const char* versions, char* error) {
  std::lock_guard lock(mutex);
  if (error) error[0] = 0;
  try {
    sdk();
    if (versions) {
      std::size_t consumed = 0;
      while (versions[consumed]) {
        const auto length = strnlen(versions + consumed, 128);
        if (length == 128 || consumed + length + 1 > 16384)
          throw std::runtime_error("Invalid Steam initialization interface list");
        if (!sdk().interface(std::string_view(versions + consumed, length)))
          throw std::runtime_error("Steam initialization requested an interface outside the detected SDK");
        consumed += length + 1;
      }
    }
    if (initialized_) return 0;
    (load_module_ ? load_module_ : platform::load_running_steam_client)();
    const auto get_export = module_export_ ? module_export_ : platform::steam_client_export;
    auto factory = reinterpret_cast<void* (*)(const char*, int*)>(get_export("CreateInterface"));
    get_callback_ = reinterpret_cast<decltype(get_callback_)>(get_export("Steam_BGetCallback"));
    free_callback_ = reinterpret_cast<decltype(free_callback_)>(get_export("Steam_FreeLastCallback"));
    get_result_ = reinterpret_cast<decltype(get_result_)>(get_export("Steam_GetAPICallResult"));
    release_tls_ = reinterpret_cast<decltype(release_tls_)>(get_export("Steam_ReleaseThreadLocalMemory"));
    client_ = factory("SteamClient023", nullptr);
    if (!client_) throw std::runtime_error("SteamClient023 unavailable");
    pipe_ = member<std::int32_t>(client_, sdk().slot("SteamAPI_ISteamClient_CreateSteamPipe"));
    if (!pipe_) throw std::runtime_error("Steam CreateSteamPipe failed");
    user_ = member<std::int32_t>(client_, sdk().slot("SteamAPI_ISteamClient_ConnectToGlobalUser"), pipe_);
    if (!user_) throw std::runtime_error("Steam ConnectToGlobalUser failed");
    const Interface *apps{}, *user{}, *utils{};
    for (const auto& type : sdk().interfaces) {
      if (type.name == "ISteamApps") apps = &type;
      if (type.name == "ISteamUser") user = &type;
      if (type.name == "ISteamUtils") utils = &type;
    }
    if (!apps || !user || !utils) throw std::logic_error("Pinned Steam SDK lacks required interfaces");
    if (!member<bool>(host_interface(*user), sdk().slot("SteamAPI_ISteamUser_BLoggedOn")))
      throw std::runtime_error("Steam account is offline; online client required");
    if (member<std::uint32_t>(host_interface(*utils), sdk().slot("SteamAPI_ISteamUtils_GetAppID")) != plan_.appid)
      throw std::runtime_error("Steam client app ID differs from launch plan");
    const bool owned = member<bool>(host_interface(*apps), sdk().slot("SteamAPI_ISteamApps_BIsSubscribedApp"), plan_.appid);
    log::info("steam", "H2 BIsSubscribedApp({})={}", plan_.appid, owned);
    if (!owned) throw std::runtime_error("Running Steam account does not own the plan's app ID");
    if (versions) {
      for (const char* p = versions; *p; p += std::strlen(p) + 1) host_interface(*sdk().interface(p));
    }
    initialized_ = true;
    ++generation_;
    log::info("steam", "H2 initialized app {} with SDK {}; overlay module loaded={}", plan_.appid, sdk().version, platform::steam_overlay_loaded());
    return 0;
  } catch (const std::exception& e) {
    log::error("steam", "Initialization refused: {}", e.what());
    if (error) { std::strncpy(error, e.what(), 1023); error[1023] = 0; }
    shutdown();
    return 1; // k_ESteamAPIInitResult_FailedGeneric
  }
}
void Bridge::shutdown() {
  std::lock_guard lock(mutex);
  if (outstanding_ && free_callback_) free_callback_(pipe_);
  outstanding_ = false;
  callback_.clear();
  if (client_ && user_) member<void>(client_, sdk().slot("SteamAPI_ISteamClient_ReleaseUser"), pipe_, user_);
  user_ = 0;
  if (client_ && pipe_) member<bool>(client_, sdk().slot("SteamAPI_ISteamClient_BReleaseSteamPipe"), pipe_);
  if (initialized_) log::info("steam", "H2 shutdown app {}", plan_.appid);
  pipe_ = 0;
  client_ = nullptr;
  initialized_ = false;
  // Retain inert handles so stale guest pointers can never alias a new session.
  release_thread();
}
GuestAddr Bridge::interface(std::string_view version) {
  std::lock_guard lock(mutex);
  if (!initialized_) return 0;
  auto* type = sdk().interface(version);
  if (!type) { log::error("steam", "Interface request outside detected SDK refused"); return 0; }
  for (const auto& [key,h] : handles_) if (h->type == type && h->generation == generation_) return key;
  auto h = std::make_unique<Handle>(Handle{host_interface(*type), type, generation_});
  const auto key = h2g(h.get());
  handles_.emplace(key, std::move(h));
  return key;
}
void* Bridge::resolve(GuestAddr handle, std::string_view classname) {
  if (!initialized_) throw std::runtime_error("Steam flat call before successful initialization");
  const auto found = handles_.find(handle);
  if (found == handles_.end() || found->second->generation != generation_ || found->second->type->name != classname)
    throw std::runtime_error("Invalid or stale Steam interface handle");
  return found->second->host;
}
void Bridge::require_pipe(std::int32_t pipe) {
  if (!initialized_ || !pipe || pipe != pipe_) throw std::runtime_error("Invalid Steam manual dispatch pipe");
}
void Bridge::run_frame(std::int32_t pipe) {
  std::lock_guard lock(mutex);
  require_pipe(pipe);
  // SDK 1.64 SteamAPI_ManualDispatch_RunFrame pumps Utils, Input(false), then
  // Controller. ISteamInput::RunFrame's false preserves Init(true)'s explicit
  // polling mode. Do not substitute ISteamClient's deprecated RunFrame here.
  for (auto name : {"ISteamUtils", "ISteamInput", "ISteamController"}) {
    auto type = std::ranges::find(sdk().interfaces, std::string_view(name), &Interface::name);
    if (type == sdk().interfaces.end()) throw std::logic_error("Steam frame interface absent from SDK");
    auto self = resolve(interface(type->version), name);
    auto slot = sdk().slot("SteamAPI_" + std::string(name) + "_RunFrame");
    if (type->name == "ISteamInput") member<void>(self, slot, false);
    else member<void>(self, slot);
  }
}
bool Bridge::next_callback(std::int32_t pipe, void* message) {
  std::lock_guard lock(mutex);
  require_pipe(pipe);
  if (!message) throw std::runtime_error("Null Steam CallbackMsg_t");
  if (outstanding_) throw std::runtime_error("Steam callback must be freed before fetching another");
  for (int skipped = 0; skipped < 1024; ++skipped) {
    HostCallback host{};
    std::uint64_t call{};
    if (!get_callback_(pipe_, &host, &call)) return false;
    const auto* layout = sdk().callback(host.id);
    if (!layout || host.bytes != static_cast<std::int32_t>(layout->host_size)) {
      if (unsupported_callbacks_.emplace(host.id, host.bytes).second)
        log::warn("steam", "Unsupported callback {} (host size {}); discarded", host.id, host.bytes);
      free_callback_(pipe_);
      continue;
    }
    callback_.resize(layout->guest_size);
    if (!host.data || !convert(*layout, callback_, {static_cast<const std::byte*>(host.data), layout->host_size}, true)) {
      free_callback_(pipe_);
      throw std::runtime_error("Steam callback conversion failed");
    }
    if (host.id == 703) { // steam_api_common.h: SteamAPICallCompleted_t
      std::int32_t id{};
      std::memcpy(&id, callback_.data() + 8, 4);
      const auto* completed = sdk().callback(id);
      if (!completed) {
        log::warn("steam", "Unsupported Steam call-result callback {}", id);
        free_callback_(pipe_);
        continue;
      }
      std::memcpy(callback_.data() + 12, &completed->guest_size, 4);
    }
    // steam_api.h: Android CallbackMsg_t has pack(4), 20 bytes, pointer at 8.
    const auto pointer = h2g(callback_.data());
    const auto size = static_cast<std::int32_t>(callback_.size());
    auto* out = static_cast<std::byte*>(message);
    std::memcpy(out, &host.user, 4); std::memcpy(out+4, &host.id, 4);
    std::memcpy(out+8, &pointer, 8); std::memcpy(out+16, &size, 4);
    outstanding_ = true;
    callback_thread_ = std::this_thread::get_id();
    return true;
  }
  throw std::runtime_error("Steam callback queue contains too many unsupported records");
}
void Bridge::free_callback(std::int32_t pipe) {
  std::lock_guard lock(mutex);
  require_pipe(pipe);
  if (!outstanding_ || callback_thread_ != std::this_thread::get_id()) throw std::runtime_error("Steam callback freed without ownership");
  free_callback_(pipe_);
  outstanding_ = false;
  callback_.clear();
}
bool Bridge::call_result(std::int32_t pipe, std::uint64_t call, void* data, std::int32_t size, std::int32_t id, bool* failed) {
  std::lock_guard lock(mutex);
  require_pipe(pipe);
  if (failed) *failed = true;
  auto* layout = sdk().callback(id);
  if (!layout || !data || size != static_cast<std::int32_t>(layout->guest_size)) {
    log::error("steam", "Call result rejected: callback {} guest size {}", id, size);
    return false;
  }
  std::vector<std::byte> host(layout->host_size);
  bool failure = true;
  if (!get_result_(pipe_, call, host.data(), static_cast<std::int32_t>(host.size()), id, &failure)) return false;
  if (!convert(*layout, {static_cast<std::byte*>(data), layout->guest_size}, host, true)) return false;
  if (failed) *failed = failure;
  return true;
}
// SDK 1.64 SteamAPI_ReleaseCurrentThreadMemory forwards false; native thread
// teardown forwards true to Steam_ReleaseThreadLocalMemory(bool thread_exit).
void Bridge::release_thread(bool exiting) { std::lock_guard lock(mutex); if (release_tls_) release_tls_(exiting); }
void release_steam_thread() { Bridge::get().release_thread(true); }

Record::Record(const Sdk& sdk, std::string_view type, void* guest, std::size_t count, bool read)
    : layout_(*[&] { auto* l = sdk.layout(type); if (!l) throw std::runtime_error("Steam record layout absent"); return l; }()), guest_(guest), count_(count) {
  if (count > 1024*1024 / layout_.host_size) throw std::runtime_error("Steam record array exceeds bridge limit");
  if (!guest) return;
  host_.resize(count * layout_.host_size);
  if (read) for (std::size_t i = 0; i < count; ++i)
    convert(layout_, {host_.data()+i*layout_.host_size,layout_.host_size},
            {static_cast<const std::byte*>(guest)+i*layout_.guest_size,layout_.guest_size}, false);
}
void Record::write_back() {
  if (!guest_) return;
  for (std::size_t i = 0; i < count_; ++i)
    convert(layout_, {static_cast<std::byte*>(guest_)+i*layout_.guest_size,layout_.guest_size},
            {host_.data()+i*layout_.host_size,layout_.host_size}, true);
}
void return_record(hle::CallContext& ctx,const Sdk& sdk,std::string_view type,const void* host,std::size_t size) {
  const auto* layout=sdk.layout(type);
  if (!layout || layout->host_size!=size) throw std::runtime_error("Invalid Steam record return layout");
  std::vector<std::byte> guest(layout->guest_size);
  convert(*layout,guest,{static_cast<const std::byte*>(host),size},true);
  // These SDK flat returns are non-HFA records. AAPCS64 6.8.2: <=16 bytes
  // use X0/X1; larger records use the caller's indirect-result pointer in X8.
  if(guest.size()>16) std::memcpy(g2h(ctx.x(8)),guest.data(),guest.size());
  else {
    std::uint64_t words[2]{}; std::memcpy(words,guest.data(),guest.size());
    ctx.return_x(words[0]); if(guest.size()>8) ctx.return_x1(words[1]);
  }
}
} // namespace venture::steam
