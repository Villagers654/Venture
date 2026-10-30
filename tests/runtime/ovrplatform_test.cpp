#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "common/log.h"
#include "ovrplatform_support.h"

#include "../async_wait.h"

using namespace venture;
using namespace venture::ovrplatform;
namespace optest = venture::ovrplatform::test;

TEST_CASE("Platform optional text on a null model preserves the request error and borrowed lifetime") {
  optest::Arena arena;
  auto provider = std::make_shared<optest::FakeProvider>();
  provider->prepare_error = failure(ErrorCode::unavailable);
  Session session(optest::app(), provider, arena.memory());
  session.initialize_async(optest::app().app_id);
  const auto message = optest::wait_pop(session);
  REQUIRE(session.get(message, "ovr_Message_IsError") == 1);
  const auto user = session.get(message, "ovr_Message_GetUser");
  REQUIRE(user == 0);
  const auto image = session.get(user, "ovr_User_GetImageUrl");
  REQUIRE(image != 0);
  CHECK(std::string(g2h<char>(image)).empty());
  CHECK(session.get(user, "ovr_User_GetSmallImageUrl") == image);
  CHECK(session.get(user, "ovr_User_GetID") == 0);
  CHECK_FALSE(session.initialized());
  const auto error = session.get(message, "ovr_Message_GetError");
  CHECK(static_cast<std::int32_t>(session.get(error, "ovr_Error_GetCode")) == -20003);
  CHECK(session.get(error, "ovr_User_GetImageUrl") == 0);  // Wrong type is still invalid.
  session.free_message(message);
  CHECK(session.get(message, "ovr_User_GetImageUrl") == 0);  // Stale nonzero handle.
  session.free_message(image);
  CHECK(std::string(g2h<char>(image)).empty());
  session.shutdown();
  CHECK(arena.live == 0);
  CHECK(session.get(0, "ovr_User_GetImageUrl") == 0);
}

TEST_CASE("Platform registry, nested arrays and notifications use one owned model graph") {
  optest::Arena arena;
  Session session(optest::app(), std::make_shared<optest::FakeProvider>(), arena.memory());
  REQUIRE(session.initialize(optest::app().app_id) == InitResult::success);
  Model user = user_model(42, "alias", "display");
  Model entry{"LeaderboardEntry",
              {{"Score", std::int64_t{-7}},
               {"ExtraData", Blob{std::byte{0}, std::byte{42}}},
               {"ExtraDataLength", std::uint32_t{2}},
               {"User", std::make_shared<const Model>(user)}}};
  Model array{
      "LeaderboardEntryArray",
      {{"NextUrl", std::string("https://graph.oculus.com/v1.20/leaderboard_entries?after=cursor")}},
      {entry, entry}};
  session.register_handler("ovr_Leaderboard_GetEntries",
                           {[array](Provider&, const RequestArgs& args, std::stop_token) {
                              CHECK(std::get<std::string>(args.values[0]) == "board");
                              CHECK(std::get<EnumValue>(args.values[2]).value == 1);
                              Result result;
                              result.model = array;
                              return result;
                            },
                            "LeaderboardEntryArray"});
  auto request = optest::invoke(session, "ovr_Leaderboard_GetEntries", {h2g("board"), 10, 1, 2});
  auto message = optest::wait_pop(session);
  CHECK(session.get(message, "ovr_Message_GetRequestID") == request);
  auto payload = session.get(message, "ovr_Message_GetLeaderboardEntryArray");
  REQUIRE(payload != 0);
  CHECK(optest::invoke(session, "ovr_LeaderboardEntryArray_GetSize", {payload}) == 2);
  auto second = optest::invoke(session, "ovr_LeaderboardEntryArray_GetElement", {payload, 1});
  REQUIRE(second != 0);
  CHECK(optest::invoke(session, "ovr_LeaderboardEntryArray_GetElement", {payload, 2}) == 0);
  CHECK(optest::invoke(session, "ovr_LeaderboardEntryArray_GetElement", {payload, UINT64_MAX}) ==
        0);
  CHECK(optest::invoke(session, "ovr_LeaderboardEntry_GetScore", {second}) ==
        static_cast<std::uint64_t>(-7));
  auto blob = session.get(second, "ovr_LeaderboardEntry_GetExtraData");
  REQUIRE(blob != 0);
  CHECK(g2h<std::byte>(blob)[1] == std::byte{42});
  auto nested = session.get(second, "ovr_LeaderboardEntry_GetUser");
  CHECK(session.get(nested, "ovr_User_GetID") == 42);
  CHECK(session.get(nested, "ovr_Product_GetName") == 0);
  CHECK(session.get(message, "ovr_Message_GetProductArray") == 0);
  CHECK(session.get(payload, "ovr_LeaderboardEntryArray_HasNextPage") == 1);
  auto page = session.snapshot_page(payload, "LeaderboardEntryArray");
  REQUIRE(page.has_value());
  CHECK_FALSE(page->next_url.empty());
  RequestArgumentSpec page_spec{"fixture", {{ArgKind::model, -1, "LeaderboardEntryArray"}}};
  std::uint64_t page_bits[] = {payload};
  auto page_args = snapshot_request_args(session, page_spec, page_bits, 8);
  REQUIRE_FALSE(page_args.error);
  CHECK(std::get<PageSnapshot>(page_args.values[0]).next_url == page->next_url);
  session.free_message(message);
  CHECK(snapshot_request_args(session, page_spec, page_bits, 8).error.code ==
        ErrorCode::invalid_argument);
  CHECK(session.get(nested, "ovr_User_GetID") == 0);
  CHECK(arena.live == 0);
  session.notify(0x12345678, user);
  message = optest::wait_pop(session);
  CHECK(session.get(message, "ovr_Message_GetRequestID") == 0);
  CHECK(session.get(message, "ovr_Message_GetType") == 0x12345678);
  CHECK(session.get(session.get(message, "ovr_Message_GetUser"), "ovr_User_GetID") == 42);
  session.free_message(message);
  session.register_handler("ovr_Leaderboard_GetEntries",
                           {[](Provider&, const RequestArgs&, std::stop_token) {
                              Result r;
                              r.model = {"User"};
                              return r;
                            },
                            "LeaderboardEntryArray"});
  session.request(optest::spec("ovr_Leaderboard_GetEntries"));
  message = optest::wait_pop(session);
  CHECK(session.get(message, "ovr_Message_IsError") == 1);
  session.free_message(message);
  session.shutdown();
  session.notify(0x12345678, user);
  CHECK(session.pop() == 0);
}

TEST_CASE("Platform typed argument snapshots own guest buffers and options") {
  optest::Arena arena;
  Session session(optest::app(), std::make_shared<optest::FakeProvider>(), arena.memory());
  auto descriptors = [&](std::string_view name) -> const RequestArgumentSpec& {
    for (auto& spec : request_argument_specs())
      if (spec.name == name) return spec;
    throw std::runtime_error("missing descriptor");
  };
  CHECK(request_argument_specs().size() == request_specs().size());
  char data[] = "bytes";
  std::uint64_t bits[] = {h2g("bucket"), h2g("key"), h2g(data), 5, UINT64_MAX, h2g("metadata")};
  auto blob = snapshot_request_args(session, descriptors("ovr_CloudStorage_Save"), bits, 8);
  REQUIRE_FALSE(blob.error);
  data[0] = 'X';
  CHECK(std::get<std::vector<std::byte>>(blob.values[2])[0] == std::byte{'b'});
  CHECK(std::get<std::int64_t>(blob.values[4]) == -1);
  char first[] = "first";
  const char* names[] = {first, "second"};
  std::uint64_t array_bits[] = {h2g(names), 2};
  auto strings =
      snapshot_request_args(session, descriptors("ovr_IAP_GetProductsBySKU"), array_bits, 8);
  REQUIRE_FALSE(strings.error);
  first[0] = 'X';
  CHECK(std::get<std::vector<std::string>>(strings.values[0])[0] == "first");
  Id ids[] = {UINT64_MAX, 42};
  std::uint64_t id_bits[] = {h2g("board"), 10, 1, h2g(ids), 2};
  auto users =
      snapshot_request_args(session, descriptors("ovr_Leaderboard_GetEntriesByIds"), id_bits, 8);
  REQUIRE_FALSE(users.error);
  ids[0] = 0;
  CHECK(std::get<std::vector<Id>>(users.values[3])[0] == UINT64_MAX);
  CHECK(std::get<std::int32_t>(users.values[1]) == 10);
  auto handle = session.create_options("GroupPresenceOptions");
  session.set_option(handle, "GroupPresenceOptions", "DestinationApiName", {"destination"});
  std::uint64_t option_bits[] = {handle};
  auto options =
      snapshot_request_args(session, descriptors("ovr_GroupPresence_Set"), option_bits, 8);
  REQUIRE_FALSE(options.error);
  session.destroy_options(handle, "GroupPresenceOptions");
  CHECK(std::get<OptionsSnapshot>(options.values[0]).fields.at("DestinationApiName")[0] ==
        "destination");
  CHECK(snapshot_request_args(session, descriptors("ovr_GroupPresence_Set"), option_bits, 8)
            .error.code == ErrorCode::invalid_argument);
  RequestArgumentSpec scalars{
      "fixture",
      {{ArgKind::id}, {ArgKind::u32}, {ArgKind::boolean}, {ArgKind::enumeration}, {ArgKind::size}}};
  std::uint64_t scalar_bits[] = {UINT64_MAX, UINT64_MAX, 0xffffff00, UINT64_MAX, 0x100000002};
  auto values = snapshot_request_args(session, scalars, scalar_bits, 8);
  REQUIRE_FALSE(values.error);
  CHECK(std::get<std::uint64_t>(values.values[0]) == UINT64_MAX);
  CHECK(std::get<std::uint32_t>(values.values[1]) == UINT32_MAX);
  CHECK_FALSE(std::get<bool>(values.values[2]));
  CHECK(std::get<EnumValue>(values.values[3]).value == -1);
  CHECK(std::get<std::uint64_t>(values.values[4]) == 0x100000002);
  array_bits[1] = UINT64_MAX;
  CHECK(snapshot_request_args(session, descriptors("ovr_IAP_GetProductsBySKU"), array_bits, 8)
            .error.code == ErrorCode::invalid_argument);
  array_bits[0] = 0;
  array_bits[1] = 1;
  CHECK(snapshot_request_args(session, descriptors("ovr_IAP_GetProductsBySKU"), array_bits, 8)
            .error.code == ErrorCode::invalid_argument);
  array_bits[1] = 0;
  CHECK_FALSE(
      snapshot_request_args(session, descriptors("ovr_IAP_GetProductsBySKU"), array_bits, 8).error);
}

TEST_CASE(
    "Platform plan identity is bound to the APK and cannot stand in for "
    "entitlement") {
  const std::filesystem::path path(VENTURE_OVR_PLAN_FIXTURE);
  struct Cleanup {
    const std::filesystem::path& path;
    ~Cleanup() {
      std::error_code error;
      std::filesystem::remove(path, error);
    }
  } cleanup{path};
  {
    std::ofstream file(path);
    file
        << R"({"appId":"3927222113966481","binaryId":"24704002429195146","package":"com.commutergames.vspeedwayalpha","versionCode":745})";
  }
  auto identity = read_app_identity(path, optest::app().package, 745);
  CHECK(identity.app_id == optest::app().app_id);
  CHECK_THROWS_AS(read_app_identity(path, "wrong.package", 745),
                  std::runtime_error);
  CHECK_THROWS_AS(read_app_identity(path, optest::app().package, 746),
                  std::runtime_error);
  {
    std::ofstream file(path);
    file << "malformed fixture that must not be echoed";
  }
  CHECK_THROWS_WITH_AS(read_app_identity(path, optest::app().package, 745),
                       "Invalid Platform launch plan", std::runtime_error);
  CHECK_FALSE(valid_id("0"));
  CHECK_FALSE(valid_id("00123"));
  CHECK_FALSE(valid_id("18446744073709551616"));
}

TEST_CASE(
    "Platform ABI: version-stable public layouts, initialization and "
    "request enums") {
  // OVR_Types.h native 1.1.71 and CAPI.cs 53/69 agree. Opaque SDK models have
  // no public struct layout. First Contact needs a future AAPCS32 adapter.
  static_assert(sizeof(KeyValuePair64) == 40);
  static_assert(sizeof(MatchmakingCriterion64) == 32);
  static_assert(sizeof(MatchmakingQuery64) == 32);
  static_assert(sizeof(OculusInitParams64) == 40);
  static_assert(sizeof(NetSyncVec3) == 12);
  static_assert(static_cast<int>(InitResult::invalid_credentials) == -8);
  CHECK(optest::spec("ovr_Entitlement_GetIsViewerEntitled").type == 0x186b58b1);
  CHECK(optest::spec("ovr_User_GetLoggedInUser").type == 0x436f345d);
  CHECK(optest::spec("ovr_User_GetAccessToken").type == 0x06a85abe);
  CHECK(optest::spec("ovr_User_GetUserProof").type == 0x22810483);
  CHECK(optest::spec("ovr_NetSync_Connect").type == 0x646d855f);
  CHECK(optest::spec("ovr_CloudStorage_Load").type == 0x40846b41);
}

TEST_CASE(
    "Platform AAPCS64: sync bool versus enum; async init and queued "
    "entitlement") {
  optest::Arena arena;
  auto provider = std::make_shared<optest::FakeProvider>();
  Session session(optest::app(), provider, arena.memory());
  CHECK(optest::invoke(session, "ovr_PopMessage") == 0);
  auto app = optest::app().app_id;
  auto request = optest::invoke(session, "ovr_UnityInitWrapperAsynchronous",
                                {h2g(app.c_str())});
  auto entitlement =
      optest::invoke(session, "ovr_Entitlement_GetIsViewerEntitled");
  REQUIRE(request != 0);
  REQUIRE(entitlement != request);
  auto init = optest::wait_pop(session);
  CHECK(optest::invoke(session, "ovr_Message_GetType", {init}) == 0x1ad307b4);
  CHECK(optest::invoke(session, "ovr_Message_GetRequestID", {init}) == request);
  auto payload =
      optest::invoke(session, "ovr_Message_GetPlatformInitialize", {init});
  CHECK(optest::invoke(session, "ovr_PlatformInitialize_GetResult",
                       {payload}) == 0);
  optest::invoke(session, "ovr_FreeMessage", {init});
  auto message = optest::wait_pop(session);
  CHECK(optest::invoke(session, "ovr_Message_GetRequestID", {message}) ==
        entitlement);
  CHECK(optest::invoke(session, "ovr_Message_IsError", {message}) == 0);
  optest::invoke(session, "ovr_FreeMessage", {message});
  CHECK(optest::invoke(session, "ovr_PlatformInitializeAndroid",
                       {h2g(app.c_str()), 0, 0}) == 0);
  CHECK(optest::invoke(session, "ovr_UnityInitWrapper", {h2g(app.c_str())}) ==
        1);
  CHECK(optest::invoke(session, "ovr_IsPlatformInitialized") == 1);
  CHECK(arena.live == 0);
}

TEST_CASE("Platform voice setters and peer queries describe a device without connections") {
  // Space Pirate Trainer DX applies connection options during startup; the
  // guest loader aborts the process for any VoIP export it receives instead.
  optest::Arena arena;
  Session session(optest::app(), std::make_shared<optest::FakeProvider>(), arena.memory());
  auto options = optest::invoke(session, "ovr_VoipOptions_Create");
  REQUIRE(options != 0);
  optest::invoke(session, "ovr_Voip_SetNewConnectionOptions", {options});
  optest::invoke(session, "ovr_Voip_SetOutputSampleRate", {48000});
  optest::invoke(session, "ovr_Voip_SetMicrophoneFilterCallback", {0x1000});
  optest::invoke(session, "ovr_Voip_SetMicrophoneFilterCallbackWithFixedSizeBuffer", {0x1000, 480});
  optest::invoke(session, "ovr_VoipOptions_Destroy", {options});
  CHECK(optest::invoke(session, "ovr_Voip_GetIsConnectionUsingDtx", {42}) == 0);
  CHECK(optest::invoke(session, "ovr_Voip_GetLocalBitrate", {42}) == 0);
  CHECK(optest::invoke(session, "ovr_Voip_GetSyncTimestampDifference", {7}) == 0);
  CHECK(optest::invoke(session, "ovr_PopMessage") == 0);
  CHECK(arena.live == 0);
}

TEST_CASE(
    "Platform messages: completion FIFO, typed errors, borrowed "
    "lifetime, invalid handles") {
  optest::Arena arena;
  auto provider = std::make_shared<optest::FakeProvider>();
  provider->blocked = true;
  Session session(optest::app(), provider, arena.memory());
  REQUIRE(session.initialize(optest::app().app_id) == InitResult::success);
  auto slow =
      session.request(optest::spec("ovr_Entitlement_GetIsViewerEntitled"));
  provider->wait_entered();
  auto fast = session.request(optest::spec("ovr_Achievements_Unlock"));
  auto message = optest::wait_pop(session);
  CHECK(session.get(message, "ovr_Message_GetRequestID") == fast);
  CHECK(session.get(message, "ovr_Message_IsError") == 1);
  CHECK(session.get(message, "ovr_Message_GetUser") == 0);
  auto error = session.get(message, "ovr_Message_GetError");
  auto borrowed = session.get(error, "ovr_Error_GetMessage");
  CHECK(static_cast<std::int32_t>(session.get(error, "ovr_Error_GetCode")) ==
        -20000);
  CHECK(std::string(g2h<char>(borrowed)) == "Not supported on this Platform.");
  provider->unblock();
  auto second = optest::wait_pop(session);
  CHECK(session.get(second, "ovr_Message_GetRequestID") == slow);
  session.free_message(second);
  CHECK(std::string(g2h<char>(borrowed)) == "Not supported on this Platform.");
  session.free_message(
      error);  // Child is borrowed; cannot free its owner's graph.
  CHECK(session.get(message, "ovr_Message_IsError") == 1);
  session.free_message(message);
  CHECK(session.get(message, "ovr_Message_GetType") == 0);
  session.free_message(message);
  CHECK(arena.live == 0);
}

TEST_CASE(
    "Platform shutdown: cancel outstanding and discard late provider success") {
  optest::Arena arena;
  auto provider = std::make_shared<optest::FakeProvider>();
  provider->blocked = true;
  Session session(optest::app(), provider, arena.memory());
  session.initialize(optest::app().app_id);
  auto request =
      session.request(optest::spec("ovr_Entitlement_GetIsViewerEntitled"));
  provider->wait_entered();
  session.shutdown();
  provider->unblock();
  auto message = optest::wait_pop(session);
  CHECK(session.get(message, "ovr_Message_GetRequestID") == request);
  auto error = session.get(message, "ovr_Message_GetError");
  CHECK(static_cast<std::int32_t>(session.get(error, "ovr_Error_GetCode")) ==
        -20006);
  session.free_message(message);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  CHECK(session.pop() == 0);
  CHECK_FALSE(session.initialized());
  CHECK(session.logged_in_user_id() == 0);
}

TEST_CASE("Platform Unity native-message unwrap preserves user and proof payloads") {
  // Blaston 203302372: MessageWithUser/GetDataFromMessage at libil2cpp
  // +0x4423c20 and MessageWithUserProof at +0x4424030 first unwrap the
  // message, then call its typed getter, then construct the managed model.
  struct ProviderWithProof : Provider {
    Error prepare() override { return {}; }
    Result entitlement(std::stop_token) override { return {}; }
    Result logged_in_user(std::stop_token) override {
      return {{}, {0xfedcba9876543210, "fixture-alias", "Fixture User"}, {}};
    }
    Result user_proof(std::stop_token) override {
      return {{}, {}, std::string(64, 'n')};
    }
  };
  optest::Arena arena;
  Session session(optest::app(), std::make_shared<ProviderWithProof>(), arena.memory());
  REQUIRE(session.initialize(optest::app().app_id) == InitResult::success);
  CHECK(optest::invoke(session, "ovr_Message_GetNativeMessage", {0}) == 0);
  for (auto name : {"ovr_User_GetLoggedInUser", "ovr_User_GetUserProof"}) {
    const auto request = optest::invoke(session, name);
    const auto message = optest::wait_pop(session);
    const auto native = optest::invoke(session, "ovr_Message_GetNativeMessage", {message});
    REQUIRE(native != 0);
    CHECK(optest::invoke(session, "ovr_Message_GetNativeMessage", {native}) == native);
    CHECK(optest::invoke(session, "ovr_Message_GetRequestID", {native}) == request);
    REQUIRE(optest::invoke(session, "ovr_Message_IsError", {native}) == 0);
    const bool user = std::string_view(name) == "ovr_User_GetLoggedInUser";
    const auto payload = optest::invoke(
        session, user ? "ovr_Message_GetUser" : "ovr_Message_GetUserProof", {native});
    REQUIRE(payload != 0);
    CHECK(optest::invoke(session, "ovr_Message_GetNativeMessage", {payload}) == 0);
    if (user)
      CHECK(optest::invoke(session, "ovr_User_GetID", {payload}) == 0xfedcba9876543210);
    else {
      const auto nonce = optest::invoke(session, "ovr_UserProof_GetNonce", {payload});
      REQUIRE(nonce != 0);
      CHECK(std::string(g2h<const char>(nonce)) == std::string(64, 'n'));
    }
    optest::invoke(session, "ovr_FreeMessage", {message});
    CHECK(optest::invoke(session, "ovr_Message_GetNativeMessage", {native}) == 0);
    CHECK(optest::invoke(session, user ? "ovr_User_GetID" : "ovr_UserProof_GetNonce", {payload}) == 0);
    CHECK(arena.live == 0);
  }
}

TEST_CASE("Platform enum ToString and FromString use the SDK's names") {
  optest::Arena arena;
  Session session(optest::app(), std::make_shared<optest::FakeProvider>(), arena.memory());
  // The Climb 2 names its leaderboard read with this after ovr_Leaderboard_GetEntries.
  auto top = optest::invoke(session, "ovrLeaderboardStartAt_ToString", {0});
  CHECK(std::string(g2h<char>(top)) == "TOP");
  auto unknown = optest::invoke(session, "ovrLeaderboardStartAt_ToString", {99});
  CHECK(std::string(g2h<char>(unknown)) == "UNKNOWN");
  const char* centered = "CENTERED_ON_VIEWER";
  CHECK(optest::invoke(session, "ovrLeaderboardStartAt_FromString", {venture::h2g(centered)}) == 1);
  // Explicit header values, not C# declaration order.
  const char* missing = "MISSING_PARAMETERS";
  CHECK(static_cast<std::int32_t>(optest::invoke(session, "ovrLivestreamingStartStatus_FromString",
                                                 {venture::h2g(missing)})) == -4);
}
