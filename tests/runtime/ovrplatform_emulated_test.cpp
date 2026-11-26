#include "ovrplatform_support.h"
#include "hle/ovrplatform/emulated.h"
#include "hle/ovrplatform/horizon.h"

#include <cstring>
#include <random>

#include "../async_wait.h"

using namespace venture::ovrplatform;
using venture::g2h;
using venture::h2g;
using namespace venture::ovrplatform::test;
namespace {
struct StateDir {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("venture-emulated-" + std::to_string(std::random_device{}()));
  ~StateDir() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};
struct Fixture {
  explicit Fixture(const std::filesystem::path& state) : session(app(), provider, arena.memory()) {
    register_emulated_handlers(session, app(), state);
    REQUIRE(session.initialize(app().app_id) == InitResult::success);
  }
  ~Fixture() {
    while (auto message = session.pop()) session.free_message(message);
  }
  // Completes one request and returns its message (caller frees it).
  Handle call(std::string_view name, RequestArgs args = {}) {
    auto id = session.request(spec(name), std::move(args));
    auto message = wait_pop(session);
    REQUIRE(session.get(message, "ovr_Message_GetRequestID") == id);
    INFO(name, " error=", session.get(message, "ovr_Message_IsError"));
    return message;
  }
  std::string text(Handle h, std::string_view getter) { return g2h<const char>(session.get(h, getter)); }
  Arena arena;
  std::shared_ptr<FakeProvider> provider = std::make_shared<FakeProvider>();
  Session session;
};
HorizonMessage wait_message(Horizon& horizon) {
  const auto until = std::chrono::steady_clock::now() + venture::test::hang_timeout;
  while (std::chrono::steady_clock::now() < until) {
    if (auto message = horizon.pop(Horizon::global_session)) return *message;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  throw std::runtime_error("Horizon completion timed out");
}
}  // namespace

TEST_CASE("Platform rooms: the player is in a solo room until the game makes another") {
  StateDir state;
  Fixture f(state.path);
  auto message = f.call("ovr_Room_GetCurrent");
  REQUIRE(f.session.get(message, "ovr_Message_IsError") == 0);
  auto room = f.session.get(message, "ovr_Message_GetRoom");
  REQUIRE(room);
  CHECK(f.session.get(room, "ovr_Room_GetType") == 4);  // ovrRoomType_Solo
  const auto solo = f.session.get(room, "ovr_Room_GetID");
  CHECK(solo != 0);
  auto users = f.session.get(room, "ovr_Room_GetUsers");
  CHECK(f.session.get(users, "ovr_UserArray_GetSize") == 1);
  CHECK(f.session.get(f.session.get(room, "ovr_Room_GetOwner"), "ovr_User_GetID") == 0xfedcba9876543210);
  f.session.free_message(message);

  OptionsSnapshot options{"RoomOptions", {{std::string("DataStore\0map", 13), {"String", "harbor"}}}};
  message = f.call("ovr_Room_CreateAndJoinPrivate2", {{EnumValue{1}, std::uint32_t{4}, options}});
  room = f.session.get(message, "ovr_Message_GetRoom");
  REQUIRE(room);
  const auto made = f.session.get(room, "ovr_Room_GetID");
  CHECK(made != solo);
  CHECK(f.session.get(room, "ovr_Room_GetType") == 3);  // Private
  CHECK(f.session.get(room, "ovr_Room_GetMaxUsers") == 4);
  CHECK(f.session.get(room, "ovr_Room_GetJoinPolicy") == 1);
  auto store = f.session.get(room, "ovr_Room_GetDataStore");
  CHECK(f.session.get(store, "ovr_DataStore_GetNumKeys") == 1);
  CHECK(std::string(g2h<const char>(f.session.get(store, "ovr_DataStore_GetKey", 0))) == "map");
  f.session.free_message(message);

  message = f.call("ovr_Room_UpdateDataStore", {{made, std::vector<std::string>{"map", "reef", "mode", "duel"}, std::uint32_t{2}}});
  store = f.session.get(f.session.get(message, "ovr_Message_GetRoom"), "ovr_Room_GetDataStore");
  CHECK(f.session.get(store, "ovr_DataStore_GetNumKeys") == 2);
  f.session.free_message(message);

  message = f.call("ovr_Room_Leave", {{made}});
  room = f.session.get(message, "ovr_Message_GetRoom");
  CHECK(f.session.get(room, "ovr_Room_GetType") == 4);
  f.session.free_message(message);
  message = f.call("ovr_Room_Get", {{made}});  // The player was its only member.
  CHECK(f.session.get(message, "ovr_Message_IsError") == 1);
  f.session.free_message(message);
}

TEST_CASE("Platform parties, matchmaking and dialogs complete without unsupported errors") {
  StateDir state;
  Fixture f(state.path);
  auto message = f.call("ovr_Party_GetCurrent");
  CHECK(f.session.get(message, "ovr_Message_IsError") == 1);  // Not in a party.
  f.session.free_message(message);
  message = f.call("ovr_Party_Create");
  const auto party = f.session.get(f.session.get(message, "ovr_Message_GetPartyID"), "ovr_PartyID_GetID");
  CHECK(party != 0);
  f.session.free_message(message);
  message = f.call("ovr_Party_GetCurrent");
  REQUIRE(f.session.get(message, "ovr_Message_IsError") == 0);
  CHECK(f.session.get(f.session.get(message, "ovr_Message_GetParty"), "ovr_Party_GetID") == party);
  f.session.free_message(message);

  OptionsSnapshot matchmaking{"MatchmakingOptions", {{"CreateRoomMaxUsers", {"8"}}}};
  message = f.call("ovr_Matchmaking_CreateAndEnqueueRoom2", {{std::string("arena"), matchmaking}});
  REQUIRE(f.session.get(message, "ovr_Message_IsError") == 0);
  auto both = f.session.get(message, "ovr_Message_GetMatchmakingEnqueueResultAndRoom");
  auto room = f.session.get(both, "ovr_MatchmakingEnqueueResultAndRoom_GetRoom");
  CHECK(f.session.get(room, "ovr_Room_GetType") == 1);
  CHECK(f.session.get(room, "ovr_Room_GetMaxUsers") == 8);
  auto queued = f.session.get(both, "ovr_MatchmakingEnqueueResultAndRoom_GetMatchmakingEnqueueResult");
  CHECK(f.text(queued, "ovr_MatchmakingEnqueueResult_GetPool") == "arena");
  f.session.free_message(message);

  message = f.call("ovr_User_LaunchBlockFlow", {{Id{5}}});
  auto block = f.session.get(message, "ovr_Message_GetLaunchBlockFlowResult");
  CHECK(f.session.get(block, "ovr_LaunchBlockFlowResult_GetDidCancel") == 1);
  CHECK(f.session.get(block, "ovr_LaunchBlockFlowResult_GetDidBlock") == 0);
  f.session.free_message(message);
  for (auto name : {"ovr_RichPresence_SetIsJoinable", "ovr_Cowatching_IsInSession", "ovr_Voip_GetMicrophoneAvailability",
                    "ovr_User_GetBlockedUsers", "ovr_Application_GetInstalledApplications"}) {
    RequestArgs args;
    if (name == std::string_view("ovr_RichPresence_SetIsJoinable")) args.values.emplace_back(true);
    message = f.call(name, std::move(args));
    CHECK_MESSAGE(f.session.get(message, "ovr_Message_IsError") == 0, name);
    f.session.free_message(message);
  }
}

TEST_CASE("Platform user data store and cloud storage persist per account and app") {
  StateDir state;
  const std::vector<std::byte> save{std::byte{1}, std::byte{0}, std::byte{0xfe}};
  {
    Fixture f(state.path);
    auto message = f.call("ovr_UserDataStore_PrivateWriteEntry", {{Id{42}, std::string("level"), std::string("7")}});
    CHECK(f.session.get(f.session.get(message, "ovr_Message_GetUserDataStoreUpdateResponse"),
                        "ovr_UserDataStoreUpdateResponse_GetSuccess") == 1);
    f.session.free_message(message);
    message = f.call("ovr_CloudStorage_Save",
                     {{std::string("saves"), std::string("slot/1"), save, std::uint32_t{3}, std::int64_t{9}, std::string("x")}});
    CHECK(f.session.get(message, "ovr_Message_IsError") == 0);
    f.session.free_message(message);
  }
  Fixture f(state.path);
  auto message = f.call("ovr_UserDataStore_PrivateGetEntryByKey", {{Id{42}, std::string("level")}});
  auto store = f.session.get(message, "ovr_Message_GetDataStore");
  REQUIRE(f.session.get(store, "ovr_DataStore_GetNumKeys") == 1);
  f.session.free_message(message);

  message = f.call("ovr_CloudStorage_Load", {{std::string("saves"), std::string("slot/1")}});
  REQUIRE(f.session.get(message, "ovr_Message_IsError") == 0);
  auto data = f.session.get(message, "ovr_Message_GetCloudStorageData");
  REQUIRE(f.session.get(data, "ovr_CloudStorageData_GetDataSize") == 3);
  auto bytes = f.session.get(data, "ovr_CloudStorageData_GetData");
  REQUIRE(bytes);
  CHECK(std::memcmp(g2h(bytes), save.data(), save.size()) == 0);
  f.session.free_message(message);

  message = f.call("ovr_CloudStorage_LoadBucketMetadata", {{std::string("saves")}});
  auto all = f.session.get(message, "ovr_Message_GetCloudStorageMetadataArray");
  REQUIRE(f.session.get(all, "ovr_CloudStorageMetadataArray_GetSize") == 1);
  auto meta = f.session.get(all, "ovr_CloudStorageMetadataArray_GetElement", 0);
  CHECK(f.text(meta, "ovr_CloudStorageMetadata_GetKey") == "slot/1");
  CHECK(f.session.get(meta, "ovr_CloudStorageMetadata_GetCounter") == 9);
  f.session.free_message(message);
}

TEST_CASE("Platform key/value arguments are read on both guest ABIs") {
  StateDir state;
  Fixture f(state.path);
  const RequestArgumentSpec* desc = nullptr;
  for (const auto& d : request_argument_specs())
    if (std::string_view(d.name) == "ovr_Room_UpdateDataStore") desc = &d;
  REQUIRE(desc);
  struct Pair64 {  // OVR_Types.h ovrKeyValuePair on AArch64
    std::uint64_t key;
    std::int32_t type;
    std::uint64_t string;
    std::int32_t integer;
    double real;
  };
  static_assert(sizeof(Pair64) == 40);
  Pair64 pairs[2]{{h2g("map"), 0, h2g("reef"), 0, 0}, {h2g("round"), 1, 0, 3, 0}};
  auto args = snapshot_request_args(f.session, *desc, std::vector<std::uint64_t>{7, h2g(pairs), 2}, 8);
  REQUIRE(!args.error);
  CHECK(std::get<std::vector<std::string>>(args.values[1]) == std::vector<std::string>{"map", "reef", "round", "3"});
}

TEST_CASE("Horizon maps request JSON to any C request and keeps subscriptions open") {
  StateDir state;
  Arena arena;
  auto provider = std::make_shared<FakeProvider>();
  Session session(app(), provider, arena.memory());
  register_emulated_handlers(session, app(), state.path);
  Horizon horizon(session);
  REQUIRE(horizon.setup(app().app_id) == InitResult::success);
  horizon.request("user_data_store", "public_write_entry", 1, R"({"user_id":"42","key":"k","value":"v"})", 1);
  auto message = wait_message(horizon);
  REQUIRE(message.status_code == 0);
  CHECK(venture::parse_json(message.data).at("success") == true);
  horizon.request("user_data_store", "public_get_entry_by_key", 1, R"({"user_id":42,"key":"k"})", 2);
  message = wait_message(horizon);
  REQUIRE(message.status_code == 0);
  CHECK(venture::parse_json(message.data).at("k") == "v");
  horizon.request("rooms", "get_current", 1, "{}", 3);
  message = wait_message(horizon);
  REQUIRE(message.status_code == 0);
  CHECK(venture::parse_json(message.data).at("type") == 4);
  const auto route = horizon.request("group_presence", "join_intent_received", 1, "{}", 4, true);
  CHECK(route >= 2);
  CHECK(horizon.count(route) == 0);
  horizon.stop(route);
  horizon.request("no_such_service", "anything", 1, "{}", 5);
  CHECK(wait_message(horizon).status_code == static_cast<int>(ErrorCode::unsupported));
  CHECK(arena.live == 0);
}
