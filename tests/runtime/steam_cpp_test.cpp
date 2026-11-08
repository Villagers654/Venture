#include "core/address_space.h"
#include "core/error.h"
#include "core/linker.h"
#include "core/thread.h"
#include "hle/steam/bridge.h"
#include "hle/steam/library.h"

#include <array>
#include <cstring>
#include <doctest/doctest.h>

namespace venture::steam {
struct BridgeTestAccess {
  BridgeTestAccess(void* client, decltype(Bridge::get_callback_) next, decltype(Bridge::free_callback_) free,
                   decltype(Bridge::get_result_) result) {
    auto& bridge = Bridge::get();
    bridge.sdk_ = &sdk164();
    bridge.client_ = client;
    bridge.get_callback_ = next;
    bridge.free_callback_ = free;
    bridge.get_result_ = result;
    bridge.pipe_ = 1;
    bridge.user_ = 2;
    ++bridge.generation_;
    bridge.initialized_ = true;
  }
  ~BridgeTestAccess() {
    auto& bridge = Bridge::get();
    bridge.initialized_ = false;
    bridge.handles_.clear();
    bridge.guest_vtables_.clear();
    bridge.sdk_ = nullptr;
    bridge.client_ = nullptr;
    bridge.get_callback_ = nullptr;
    bridge.free_callback_ = nullptr;
    bridge.get_result_ = nullptr;
    bridge.pipe_ = bridge.user_ = 0;
  }
};

namespace {
constexpr std::int32_t web_auth_response = 168;
constexpr std::uint32_t fake_handle = 42;
constexpr std::uint64_t fake_call = 51;
std::array<void*, 128> client_slots{}, user_slots{}, utils_slots{}, input_slots{}, controller_slots{};
void** client = client_slots.data();
void** user = user_slots.data();
void** utils = utils_slots.data();
void** input = input_slots.data();
void** controller = controller_slots.data();
std::array<std::uint32_t, 643> response{};  // GetTicketForWebApiResponse_t, 2572 bytes
struct Completion { std::uint64_t call; std::int32_t id; std::uint32_t bytes; };
Completion completion{fake_call, web_auth_response, sizeof(response)};
bool queued, queued_result;
unsigned delivered;
GuestAddr find_user_thunk;

void* get_user(void*, int, int, const char*) { return &user; }
void* get_utils(void*, int, const char*) { return &utils; }
void* get_input(void*, int, int, const char*) { return &input; }
void* get_controller(void*, int, int, const char*) { return &controller; }
void frame(void*) {}
void input_frame(void*, bool) {}
std::uint32_t request_ticket(void*, const char*) {
  response[0] = fake_handle;
  response[1] = 1;
  queued = true;
  return fake_handle;
}
bool next_callback(int, HostCallback* message, std::uint64_t*) {
  if (queued_result) {
    *message = {2, 703, &completion, sizeof(completion)};
    return true;
  }
  if (!queued) return false;
  *message = {2, web_auth_response, response.data(), sizeof(response)};
  return true;
}
void free_callback(int) { queued = queued_result = false; }
bool get_result(int, std::uint64_t call, void* payload, std::int32_t size, std::int32_t id, bool* failed) {
  if (call != fake_call || size != sizeof(response) || id != web_auth_response) return false;
  std::memcpy(payload, response.data(), sizeof(response));
  *failed = false;
  return true;
}
void initialize_context(GuestAddr out) {
  *g2h<GuestAddr>(out) = GuestThread::adopt_current().call(find_user_thunk, {2, h2g("SteamUser023")});
}
void on_auth(GuestAddr, const std::uint32_t* payload) {
  if (payload[0] == fake_handle && payload[1] == 1) ++delivered;
}
void on_result(GuestAddr object, const std::uint32_t* payload, bool failed, std::uint64_t call) {
  if (!failed && call == fake_call) on_auth(object, payload);
}
int auth_size(GuestAddr) { return sizeof(response); }
} // namespace

TEST_CASE("Steam C++ client context forwards the authentication callback to Android") {
  const auto& sdk = sdk164();
  client_slots[sdk.slot("SteamAPI_ISteamClient_GetISteamUser")] = reinterpret_cast<void*>(get_user);
  client_slots[sdk.slot("SteamAPI_ISteamClient_GetISteamUserStats")] = reinterpret_cast<void*>(get_user);
  client_slots[sdk.slot("SteamAPI_ISteamClient_GetISteamApps")] = reinterpret_cast<void*>(get_user);
  client_slots[sdk.slot("SteamAPI_ISteamClient_GetISteamUtils")] = reinterpret_cast<void*>(get_utils);
  client_slots[sdk.slot("SteamAPI_ISteamClient_GetISteamInput")] = reinterpret_cast<void*>(get_input);
  client_slots[sdk.slot("SteamAPI_ISteamClient_GetISteamController")] = reinterpret_cast<void*>(get_controller);
  const auto auth_slot = sdk.slot("SteamAPI_ISteamUser_GetAuthTicketForWebApi");
  user_slots[auth_slot] = reinterpret_cast<void*>(request_ticket);
  utils_slots[sdk.slot("SteamAPI_ISteamUtils_RunFrame")] = reinterpret_cast<void*>(frame);
  input_slots[sdk.slot("SteamAPI_ISteamInput_RunFrame")] = reinterpret_cast<void*>(input_frame);
  controller_slots[sdk.slot("SteamAPI_ISteamController_RunFrame")] = reinterpret_cast<void*>(frame);
  queued = queued_result = false;
  delivered = 0;

  HleLibrary library("libsteam_api.so");
  add_exports(library, sdk);
  BridgeTestAccess host(&client, next_callback, free_callback, get_result);
  find_user_thunk = library.hle_symbol("SteamInternal_FindOrCreateUserInterface");
  auto& thread = GuestThread::adopt_current();
  REQUIRE(thread.call(find_user_thunk, {0, h2g("SteamUtils010")}) != 0);
  CHECK(thread.call(find_user_thunk, {0, h2g("SteamUser023")}) == 0);
  const auto stats = thread.call(find_user_thunk, {2, h2g("STEAMUSERSTATS_INTERFACE_VERSION013")});
  REQUIRE(stats != 0);
  CHECK(*g2h<GuestAddr>(stats) == 0);
  const auto apps = thread.call(find_user_thunk, {2, h2g("STEAMAPPS_INTERFACE_VERSION009")});
  REQUIRE(apps != 0);
  const auto* apps_table = g2h<GuestAddr>(*g2h<GuestAddr>(apps));
  const auto unsupported_slot = sdk.slot("SteamAPI_ISteamApps_MarkContentCorrupt");
  REQUIRE(apps_table[unsupported_slot] != 0);
  CHECK(thread.call(apps_table[unsupported_slot], {apps, 1, 0}) == 0);
  CHECK(*thread.errno_location() == err::kEIO);
  const auto block = AddressSpace::get().allocate(kGuestPageSize, kProtRead | kProtWrite, "[steam-test]");
  struct Block { GuestAddr address; ~Block() { AddressSpace::get().free(address); } } memory{block};
  auto* context = g2h<GuestAddr>(block);
  context[0] = hle::make_thunk("steam-test", "context initializer", hle::wrap<&initialize_context>());
  REQUIRE(thread.call(library.hle_symbol("SteamInternal_ContextInit"), {block}) == block + 16);
  REQUIRE(context[2] != 0);
  const auto* table = g2h<GuestAddr>(*g2h<GuestAddr>(context[2]));
  REQUIRE(table[auth_slot] != 0);

  const auto callback = block + 32;
  auto* callback_table = g2h<GuestAddr>(block + 64);
  *g2h<GuestAddr>(callback) = h2g(callback_table);
  callback_table[0] = hle::make_thunk("steam-test", "authentication callback", hle::wrap<&on_auth>());
  callback_table[1] = hle::make_thunk("steam-test", "asynchronous result", hle::wrap<&on_result>());
  callback_table[2] = hle::make_thunk("steam-test", "callback size", hle::wrap<&auth_size>());
  thread.call(library.hle_symbol("SteamAPI_RegisterCallback"), {callback, web_auth_response});
  const auto pump = library.hle_symbol("SteamAPI_RunCallbacks");
  CHECK(thread.call(table[auth_slot], {context[2], h2g("fixture")}) == fake_handle);
  thread.call(pump, {});
  CHECK(delivered == 1);
  CHECK_FALSE(queued);
  thread.call(library.hle_symbol("SteamAPI_UnregisterCallback"), {callback});
  CHECK(thread.call(table[auth_slot], {context[2], h2g("fixture")}) == fake_handle);
  thread.call(pump, {});
  CHECK(delivered == 1);
  CHECK_FALSE(queued);
  thread.call(library.hle_symbol("SteamAPI_RegisterCallResult"), {callback, fake_call});
  queued_result = true;
  thread.call(pump, {});
  CHECK(delivered == 2);
  CHECK_FALSE(queued_result);
  queued_result = true;
  thread.call(pump, {});
  CHECK(delivered == 2);
}
} // namespace venture::steam
