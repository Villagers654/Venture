#include "hle/steam/bridge.h"

#include "common/log.h"
#include "core/address_space.h"
#include "core/linker.h"
#include "core/thread.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace venture::steam {
namespace {
struct CallbackObject {
  GuestAddr vtable;
  std::uint8_t flags;
  std::int32_t id;
};
static_assert(offsetof(CallbackObject, id) == 12 && sizeof(CallbackObject) == 16);
constexpr std::uint8_t registered = 1, game_server = 2;
constexpr std::int32_t call_completed = 703;

struct Registration {
  std::int32_t id;
  std::uint64_t call;
  std::uint64_t generation;
  std::uint64_t serial;
};
std::recursive_mutex callback_mutex;
std::map<GuestAddr, Registration> registrations;
std::uint64_t next_serial = 0;

bool guest_range(GuestAddr address, std::size_t size, std::uint32_t access) {
  MemoryRegion region;
  return address && AddressSpace::get().find(address, &region) &&
         (region.prot & access) == access && size <= region.size - (address - region.start);
}

GuestAddr context_init(GuestAddr address) {
  if (!guest_range(address, 3 * sizeof(GuestAddr), kProtRead | kProtWrite)) return 0;
  std::lock_guard context_lock(callback_mutex);
  auto& bridge = Bridge::get();
  std::lock_guard lock(bridge.mutex);
  auto* context = g2h<GuestAddr>(address);
  const auto generation = bridge.context_generation();
  const auto value = address + 2 * sizeof(GuestAddr);
  if (context[1] != generation) {
    context[2] = 0;
    if (generation && AddressSpace::get().is_guest_executable(context[0]))
      GuestThread::adopt_current().call(context[0], {value});
    context[1] = generation;
  }
  return value;
}

void register_callback(GuestAddr address, std::int32_t id) {
  std::lock_guard lock(callback_mutex);
  if (!guest_range(address, sizeof(CallbackObject), kProtRead | kProtWrite)) return;
  auto& callback = *g2h<CallbackObject>(address);
  if (callback.flags & game_server) return;
  callback.flags |= registered;
  callback.id = id;
  registrations.insert_or_assign(address, Registration{id, 0, 0, ++next_serial});
}

void register_result(GuestAddr address, std::uint64_t call) {
  std::lock_guard lock(callback_mutex);
  if (!call || !guest_range(address, sizeof(CallbackObject), kProtRead | kProtWrite)) return;
  auto& callback = *g2h<CallbackObject>(address);
  if (callback.flags & game_server) return;
  auto& bridge = Bridge::get();
  std::lock_guard bridge_lock(bridge.mutex);
  if (!bridge.ready()) return;
  callback.flags |= registered;
  registrations.insert_or_assign(address, Registration{callback.id, call, bridge.context_generation(), ++next_serial});
}

void unregister_result(GuestAddr address, std::uint64_t call) {
  std::lock_guard lock(callback_mutex);
  auto entry = registrations.find(address);
  if (entry == registrations.end() || entry->second.call != call) return;
  registrations.erase(entry);
  if (guest_range(address, sizeof(CallbackObject), kProtRead | kProtWrite))
    g2h<CallbackObject>(address)->flags &= ~registered;
}

void unregister_callback(GuestAddr address) { unregister_result(address, 0); }

bool dispatch(GuestAddr address, std::span<std::byte> data, bool failure, std::uint64_t call) {
  if (!guest_range(address, sizeof(CallbackObject), kProtRead)) return false;
  const auto table = g2h<CallbackObject>(address)->vtable;
  if (!guest_range(table, 3 * sizeof(GuestAddr), kProtRead)) return false;
  const auto* slots = g2h<GuestAddr>(table);
  const auto run = slots[call ? 1 : 0];
  if (!AddressSpace::get().is_guest_executable(run) || !AddressSpace::get().is_guest_executable(slots[2])) return false;
  auto& thread = GuestThread::adopt_current();
  if (thread.call(slots[2], {address}) != data.size()) return false;
  if (call) thread.call(run, {address, h2g(data.data()), failure, call});
  else thread.call(run, {address, h2g(data.data())});
  return true;
}

void run_callbacks() {
  std::lock_guard lock(callback_mutex);
  static thread_local bool pumping = false;
  if (pumping) return;
  struct Reset { bool& flag; ~Reset() { flag = false; } } reset{pumping};
  pumping = true;
  auto& bridge = Bridge::get();
  std::uint64_t generation;
  {
    std::lock_guard bridge_lock(bridge.mutex);
    if (!bridge.ready()) return;
    generation = bridge.context_generation();
    bridge.run_frame(bridge.pipe());
  }
  for (int i = 0; i < 1024; ++i) {
    std::int32_t id;
    std::uint64_t call = 0;
    bool failure = false;
    std::vector<std::byte> data;
    {
      std::lock_guard bridge_lock(bridge.mutex);
      if (bridge.context_generation() != generation) return;
      const auto pipe = bridge.pipe();
      std::array<std::byte, 20> message{}; // Android CallbackMsg_t
      if (!bridge.next_callback(pipe, message.data())) return;
      struct Free { Bridge& bridge; std::int32_t pipe; ~Free() { bridge.free_callback(pipe); } } free{bridge, pipe};
      GuestAddr payload;
      std::int32_t size;
      std::memcpy(&id, message.data() + 4, 4);
      std::memcpy(&payload, message.data() + 8, 8);
      std::memcpy(&size, message.data() + 16, 4);
      if (id == call_completed) {
        const auto* completed = g2h<const std::byte>(payload);
        std::memcpy(&call, completed, 8);
        std::memcpy(&id, completed + 8, 4);
        std::memcpy(&size, completed + 12, 4);
        data.resize(size);
        if (!call || !bridge.call_result(pipe, call, data.data(), size, id, &failure)) continue;
      } else {
        const auto* begin = g2h<const std::byte>(payload);
        data.assign(begin, begin + size);
      }
    }
    std::vector<std::pair<GuestAddr, std::uint64_t>> listeners;
    for (const auto& [address, entry] : registrations) {
      if (entry.id == id && entry.call == call && (!call || entry.generation == generation))
        listeners.emplace_back(address, entry.serial);
    }
    for (const auto& [address, serial] : listeners) {
      {
        std::lock_guard bridge_lock(bridge.mutex);
        if (bridge.context_generation() != generation) return;
      }
      auto entry = registrations.find(address);
      if (entry == registrations.end() || entry->second.serial != serial) continue;
      if (call) unregister_result(address, call);
      if (dispatch(address, data, failure, call) && (id == 163 || id == 168))
        log::info("steam", "Delivered Steam authentication callback {} to C++ listener", id);
    }
  }
}

void unsupported(hle::CallContext& ctx) {
  throw std::runtime_error("Unsupported Steam C++ method: " + std::string(ctx.info().name));
}
} // namespace

void add_cpp_exports(HleLibrary& library, const Sdk& sdk) {
  for (const auto& face : sdk.interfaces) {
    if (std::ranges::find(sdk.cpp_interfaces, face.name) == sdk.cpp_interfaces.end()) continue;
    const auto prefix = "SteamAPI_" + std::string(face.name) + "_";
    std::vector<GuestAddr> table;
    for (const auto& method : sdk.methods) {
      if (!method.symbol.starts_with(prefix)) continue;
      if (table.size() <= method.slot) table.resize(method.slot + 1);
      auto thunk = library.hle_symbol(method.symbol);
      if (!thunk) thunk = hle::make_thunk("libsteam_api.so", method.symbol, unsupported);
      table[method.slot] = thunk;
    }
    Bridge::get().set_guest_vtable(face.version, std::move(table));
  }
#define EXPORT(name, fn) library.add_function(name, hle::make_thunk("libsteam_api.so", name, hle::wrap<&fn>()))
  EXPORT("SteamInternal_ContextInit", context_init);
  EXPORT("SteamAPI_RegisterCallback", register_callback);
  EXPORT("SteamAPI_UnregisterCallback", unregister_callback);
  EXPORT("SteamAPI_RegisterCallResult", register_result);
  EXPORT("SteamAPI_UnregisterCallResult", unregister_result);
  EXPORT("SteamAPI_RunCallbacks", run_callbacks);
#undef EXPORT
}
} // namespace venture::steam
