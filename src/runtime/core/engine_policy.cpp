#include "core/engine_policy.h"
#include "common/log.h"
#include "core/address_space.h"
#include "core/guest.h"
#include "core/guest_storage.h"
#include "core/hle.h"
#include "core/linker.h"
#include "core/process.h"
#include "core/os_quit.h"
#include "core/thread.h"
#include "cpu/cpu.h"
#include "platform/memory.h"
#include <atomic>
#include <array>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>

namespace venture::engine_policy {
namespace {

std::atomic<bool> space_warp_active{false};
std::atomic<bool> declined_logged{false};

// Unity Oculus SDK: OVRManager.SetSpaceWarp(bool) forwards the request to the
// XR provider and then records m_SpaceWarpEnabled whatever the provider did.
// Titles pace from that flag (AC Nexus spins every frame to half the display
// rate and renders motion vectors), so on a runtime without space warp they
// would run at half rate with no synthesized frames. A Quest always has space
// warp; on Venture a request takes effect only while space warp is active,
// and the SDK then records the state the runtime really has.
std::uint32_t space_warp_request(std::uint32_t enabled) {
  const bool requested = enabled & 1;
  const bool granted = requested && space_warp_active.load(std::memory_order_relaxed);
  if (requested && !granted && !declined_logged.exchange(true))
    log::info("engine", "OVRManager.SetSpaceWarp(true): space warp is not active on this host; the SDK records it off");
  return granted;
}

struct Il2Cpp {
  GuestThread& thread;
  Library* library;
  explicit Il2Cpp(GuestThread& t) : thread(t), library(Linker::get().open("libil2cpp.so", kRtldNoLoad | kRtldNow, nullptr)) {}
  std::uint64_t call(const char* name, std::initializer_list<std::uint64_t> args) {
    auto fn = Linker::get().symbol(library, name);
    if (!fn) throw std::runtime_error(std::string("missing ") + name);
    return thread.call(fn, args);
  }
  GuestAddr pointer(GuestAddr p) {
    std::uint64_t v = 0;
    if (!p || !platform::read_memory(p, &v, sizeof v)) throw std::runtime_error("unreadable IL2CPP result");
    return v;
  }
  std::string text(GuestAddr p) {
    std::string value;
    for (unsigned i = 0; p && i < 256; ++i) {
      char c;
      if (!platform::read_memory(p + i, &c, 1)) throw std::runtime_error("unreadable IL2CPP string");
      if (!c) return value;
      value += c;
    }
    throw std::runtime_error("IL2CPP string too long");
  }
  // A static method by class and name, or 0 when the title does not ship it.
  GuestAddr method(const char* ns, const char* type, const char* name, int params) {
    GuestStorage count, a, b, c;
    count.resize(8);
    std::memset(g2h<void>(count.address()), 0, 8);
    a.string(ns); b.string(type); c.string(name);
    auto assemblies = call("il2cpp_domain_get_assemblies", {call("il2cpp_domain_get", {}), count.address()});
    auto n = pointer(count.address());
    if (n > 4096) throw std::runtime_error("assembly count exceeds limit");
    for (std::uint64_t i = 0; i < n; ++i) {
      auto image = call("il2cpp_assembly_get_image", {pointer(assemblies + i * 8)});
      if (auto klass = call("il2cpp_class_from_name", {image, a.address(), b.address()}))
        return call("il2cpp_class_get_method_from_name", {klass, c.address(), static_cast<std::uint64_t>(params)});
    }
    return 0;
  }
};

// Instructions that cannot run from another address unchanged.
bool pc_relative(std::uint32_t w) {
  return (w & 0x7c000000u) == 0x14000000u ||  // B, BL
         (w & 0xff000010u) == 0x54000000u ||  // B.cond
         (w & 0x7e000000u) == 0x34000000u ||  // CBZ, CBNZ
         (w & 0x7e000000u) == 0x36000000u ||  // TBZ, TBNZ
         (w & 0x1f000000u) == 0x10000000u ||  // ADR, ADRP
         (w & 0x3b000000u) == 0x18000000u;    // LDR (literal), LDRSW, PRFM
}
// Entry hooks: the function's first instruction becomes SVC #kSvcEntryHook. The
// handler runs the hook's HLE function, then resumes at the hook's relay, which
// executes the displaced instruction and jumps back past it. The relay may sit
// anywhere: IL2CPP's neighbourhood is often reserved well beyond branch range.
struct EntryHook {
  hle::FunctionId id;
  GuestAddr relay;
};
std::mutex hooks_mutex;
std::map<GuestAddr, EntryHook> hooks;

// Null on success, else why the entry cannot be hooked.
const char* hook_entry(GuestAddr entry, hle::FunctionId id) {
  auto& space = AddressSpace::get();
  MemoryRegion text;
  std::uint32_t first = 0;
  if (!space.find(entry, &text) || !(text.prot & kProtExec)) return "not in guest executable memory";
  if (!platform::read_memory(entry, &first, 4)) return "entry unreadable";
  if (pc_relative(first)) return "entry instruction is PC-relative";
  const auto relay = space.map(0, kGuestPageSize, kProtRead | kProtWrite, kMapPrivate | kMapAnonymous, nullptr, 0,
                               "[venture engine policy]");
  if (relay < 0) return "no memory for the relay";
  const std::uint64_t back = entry + 4;
  const std::uint32_t code[] = {
      first,                           // displaced entry instruction
      0x58000071u,                     // ldr  x17, #12 (X17: intra-procedure scratch)
      0xd61f0220u,                     // br   x17
      0xd503201fu,                     // nop  (aligns the literal)
      static_cast<std::uint32_t>(back), static_cast<std::uint32_t>(back >> 32)};
  std::memcpy(g2h<void>(static_cast<GuestAddr>(relay)), code, sizeof code);
  space.protect(static_cast<GuestAddr>(relay), kGuestPageSize, kProtRead | kProtExec);
  {
    std::lock_guard lock(hooks_mutex);
    hooks[entry] = {id, static_cast<GuestAddr>(relay)};
  }
  const auto page = entry & ~(kGuestPageSize - 1);
  if (space.protect(page, kGuestPageSize, text.prot | kProtWrite) < 0) return "entry page not writable";
  const std::uint32_t svc = 0xd4000001u | (kSvcEntryHook << 5);
  std::memcpy(g2h<void>(entry), &svc, 4);
  space.protect(page, kGuestPageSize, text.prot);
  cpu_backend().invalidate(entry, 4, InvalidationCause::code_patch);
  return nullptr;
}

void install_unity_oculus(GuestThread& thread) {
  Il2Cpp api(thread);
  if (!api.library) return;  // Not an IL2CPP title.
  auto method = api.method("", "OVRManager", "SetSpaceWarp", 1);
  if (!method) return;  // Unity title without the Oculus SDK space warp API.
  auto name = api.call("il2cpp_type_get_name", {api.call("il2cpp_method_get_param", {method, 0})});
  auto param = api.text(name);
  api.call("il2cpp_free", {name});
  auto entry = api.pointer(method);
  if (param != "System.Boolean" || !entry) {
    log::warn("engine", "OVRManager.SetSpaceWarp has an unexpected shape ({}); space warp policy not applied", param);
    return;
  }
  static const auto id = hle::register_function("venture", "OVRManager.SetSpaceWarp policy", hle::wrap<&space_warp_request>());
  if (const char* why = hook_entry(entry, id)) {
    log::warn("engine", "OVRManager.SetSpaceWarp entry at {:#x} cannot be hooked ({}); space warp policy not applied",
              entry, why);
    return;
  }
  log::info("engine", "Unity Oculus SDK: OVRManager.SetSpaceWarp follows the session's space warp state");
}

struct CryEnginePacing {
  GuestAddr get_cvar = 0, execute = 0, display = 0, get_line = 0;
  std::mutex mutex;
  std::chrono::steady_clock::time_point next{};
  bool fixed_step_logged = false, error_logged = false;
  bool owns_cap = false;
  float previous_cap = 0;
};

// Read through the public console query interface: release SDKs can hide or
// shuffle ICVar's virtual methods. DisplayVarValue/GetLineNo keep that private
// ABI inside the engine. No private vtable slots, layouts or code patterns.
float cvar_float(GuestThread& thread, const CryEnginePacing& policy,
                 GuestAddr console, std::string_view key) {
  GuestStorage name, buffer;
  name.string(key);
  const auto variable = thread.call(policy.get_cvar, {console, name.address()});
  if (!variable)
    throw std::runtime_error("CryEngine ICVar unavailable");
  thread.call(policy.display, {console, variable});
  constexpr unsigned capacity = 1024;
  buffer.resize(capacity);
  std::memset(g2h<void>(buffer.address()), 0, capacity);
  if (!thread.call(policy.get_line, {console, 0, buffer.address(), capacity}))
    throw std::runtime_error("CryEngine console query output unavailable");
  const auto* bytes = g2h<const char>(buffer.address());
  std::string line;
  for (unsigned i = 0; i + 1 < capacity && bytes[i]; ++i) {
    if (bytes[i] == '$' && bytes[i + 1] >= '0' && bytes[i + 1] <= '9') { ++i; continue; }
    line += bytes[i];
  }
  std::string_view text = line;
  while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
  if (!text.starts_with(key)) throw std::runtime_error("CryEngine console query was superseded");
  text.remove_prefix(key.size());
  while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
  if (text.empty() || text.front() != '=') throw std::runtime_error("CryEngine console value unavailable");
  text.remove_prefix(1);
  while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
  float value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || (parsed.ptr != text.data() + text.size() &&
      *parsed.ptr != ' ' && *parsed.ptr != '\t'))
    throw std::runtime_error("CryEngine console value is not numeric");
  return value;
}

void cryengine_pacing(hle::CallContext& context) {
  // Console queries can write the engine log. Once task removal starts, do
  // no new SDK housekeeping: the app must be able to save and become quiet.
  if (os_quit::host_requested.load(std::memory_order_acquire) || os_quit::requested()) return;
  auto& policy = *static_cast<CryEnginePacing*>(context.user());
  std::unique_lock lock(policy.mutex, std::try_to_lock);
  if (!lock.owns_lock()) return;  // Includes reentry from a console callback.
  const auto now = std::chrono::steady_clock::now();
  if (now < policy.next) return;
  policy.next = now + std::chrono::seconds(1);
  try {
    auto& thread = context.thread();
    const auto console = context.x(0);
    GuestStorage name;
    const auto fixed = cvar_float(thread, policy, console, "t_FixedStep");
    const auto cap = cvar_float(thread, policy, console, "sys_MaxFPS");
    if (!std::isfinite(fixed) || fixed != 0) {
      if (policy.owns_cap && std::isfinite(cap) && cap < 0) {
        name.string("sys_MaxFPS " + std::to_string(policy.previous_cap));
        thread.call(policy.execute, {console, name.address(), 0, 0});
      }
      policy.owns_cap = false;
      if (!policy.fixed_step_logged) {
        log::info("engine", "CryEngine VR: fixed timestep {}; retaining the engine frame cap", fixed);
        policy.fixed_step_logged = true;
      }
      return;
    }
    if (!std::isfinite(cap)) return;
    // 0 also enables automatic menu throttling in CryEngine. -1 delegates all
    // render pacing to xrWaitFrame/VrApi, which also permits an uncapped host.
    if (cap < 0) return;
    name.string("sys_MaxFPS -1");
    thread.call(policy.execute, {console, name.address(), 0, 0});
    if (cvar_float(thread, policy, console, "sys_MaxFPS") < 0) {
      policy.previous_cap = cap;
      policy.owns_cap = true;
      log::info("engine", "CryEngine VR: render cap {} released; cadence follows the host frame wait (variable game timestep retained)", cap);
    }
  } catch (const std::runtime_error& error) {
    if (!policy.error_logged) {
      log::warn("engine", "CryEngine VR pacing SDK unavailable: {}", error.what());
      policy.error_logged = true;
    }
  }
}

void install_cryengine() {
  auto& linker = Linker::get();
  bool present = false;
  linker.for_each_image([&](const Library& library) {
    if (library.soname() == "libCrySystem.so") present = true;
  });
  if (!present) return;
  auto* library = linker.open("libCrySystem.so", kRtldNoLoad | kRtldNow | kRtldNoDelete, nullptr);
  if (!library) return;
  auto update = library->find_symbol("_ZN9CXConsole6UpdateEv");
  auto query = library->find_symbol("_ZNK9CXConsole7GetCVarEPKc");
  if (!query) query = library->find_symbol("_ZN9CXConsole7GetCVarEPKc");
  auto execute = library->find_symbol("_ZN9CXConsole13ExecuteStringEPKcbb");
  auto display = library->find_symbol("_ZN9CXConsole15DisplayVarValueEP5ICVar");
  auto get_line = library->find_symbol("_ZNK9CXConsole9GetLineNoEiPci");
  if (!update || !query || !execute || !display || !get_line) return;
  // The SDK library is retained with RTLD_NODELETE while its entry is hooked.
  // No private game symbols, package names, instruction patterns or offsets.
  auto* policy = new CryEnginePacing;
  policy->get_cvar = query;
  policy->execute = execute;
  policy->display = display;
  policy->get_line = get_line;
  const auto id = hle::register_function("venture", "CryEngine host frame pacing", cryengine_pacing, policy);
  if (const char* why = hook_entry(update, id)) {
    log::warn("engine", "CryEngine VR console entry cannot be hooked ({}); engine pacing retained", why);
    return;
  }
  log::info("engine", "CryEngine VR SDK: variable-timestep render pacing follows the host runtime");
}

}  // namespace

bool entry_hook(GuestThread& thread) {
  auto& cpu = thread.cpu();
  const GuestAddr entry = cpu.pc() - 4;  // an SVC exit leaves PC on the next instruction
  EntryHook hook;
  {
    std::lock_guard lock(hooks_mutex);
    auto it = hooks.find(entry);
    if (it == hooks.end()) return false;
    hook = it->second;
  }
  hle::dispatch(thread, hook.id);
  cpu.set_pc(hook.relay);
  return true;
}

void on_xr_session(GuestThread& thread, bool space_warp) {
  space_warp_active.store(space_warp, std::memory_order_relaxed);
  if (process_config().isa() != GuestIsa::a64) return;  // A32 IL2CPP titles predate space warp.
  static std::once_flag once;
  std::call_once(once, [&] {
    try {
      install_unity_oculus(thread);
    } catch (const std::exception& e) {
      log::warn("engine", "engine SDK policy lookup failed: {}", e.what());
    }
  });
  on_vrapi_session(thread);
}

void on_vrapi_session(GuestThread&) {
  if (process_config().isa() != GuestIsa::a64) return;
  static std::once_flag once;
  std::call_once(once, [] {
    try { install_cryengine(); }
    catch (const std::runtime_error& error) {
      log::warn("engine", "CryEngine VR SDK lookup failed: {}", error.what());
    }
  });
}

}  // namespace venture::engine_policy
