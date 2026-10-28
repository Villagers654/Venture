#include "ovrplatform_support.h"
#include "common/process.h"
#include <nlohmann/json.hpp>
#include "../async_wait.h"

using namespace venture::ovrplatform;
namespace optest = venture::ovrplatform::test;
namespace {
class PresenceProvider final : public Provider {
 public:
  Error prepare() override {
    std::unique_lock lock(mutex);
    prepare_entered = true; wake.notify_all();
    wake.wait(lock, [&] { return !prepare_blocked; });
    return {};
  }
  Result entitlement(std::stop_token) override { return {}; }
  Result heartbeat(std::string_view state, std::stop_token) override {
    std::lock_guard lock(mutex);
    states.push_back(state.empty() ? nlohmann::json::object() : nlohmann::json::parse(state));
    wake.notify_all();
    return {{}, {}, interval};
  }
  void wait(std::size_t n) {
    std::unique_lock lock(mutex);
    const bool ready = wake.wait_for(lock, venture::test::hang_timeout, [&] { return states.size() >= n; });
    INFO("presence states expected=", n, " received=", states.size(),
         " prepare entered=", prepare_entered, " blocked=", prepare_blocked);
    REQUIRE(ready);
  }
  std::mutex mutex;
  std::condition_variable wake;
  bool prepare_blocked = false, prepare_entered = false;
  std::string interval = "60";
  std::vector<nlohmann::json> states;
};
void completion(Session& session, Request id, std::uint32_t type, bool error = false,
                std::string_view error_text = {}) {
  auto message = optest::wait_pop(session);
  CHECK(session.get(message, "ovr_Message_GetRequestID") == id);
  CHECK(session.get(message, "ovr_Message_GetType") == type);
  CHECK(bool(session.get(message, "ovr_Message_IsError")) == error);
  if (!error_text.empty()) {
    const auto e = session.get(message, "ovr_Message_GetError");
    CHECK(std::string(venture::g2h<char>(session.get(e, "ovr_Error_GetMessage"))) == error_text);
  }
  session.free_message(message);
}
}
TEST_CASE("Platform presence snapshots remain ordered, partial setters preserve state and clear resets it") {
  optest::Arena arena;
  auto provider = std::make_shared<PresenceProvider>();
  Session session(optest::app(), provider, arena.memory());
  REQUIRE(session.initialize(optest::app().app_id) == InitResult::success);
  auto options = optest::invoke(session, "ovr_GroupPresenceOptions_Create");
  optest::invoke(session, "ovr_GroupPresenceOptions_SetDestinationApiName", {options, venture::h2g("menu")});
  optest::invoke(session, "ovr_GroupPresenceOptions_SetIsJoinable", {options, 1});
  auto id = optest::invoke(session, "ovr_GroupPresence_Set", {options});
  optest::invoke(session, "ovr_GroupPresenceOptions_Destroy", {options});
  completion(session, id, 0x675f5c24);
  for (auto [name, type, argument] : {std::tuple{"ovr_GroupPresence_SetLobbySession", 0x48ff55beu, venture::h2g("lobby")},
       {"ovr_GroupPresence_SetMatchSession", 0x314c84b8u, venture::h2g("match")},
       {"ovr_GroupPresence_SetDeeplinkMessageOverride", 0x521adf0du, venture::h2g("deeplink")},
       {"ovr_GroupPresence_SetDestination", 0x4c5b268au, venture::h2g("race")},
       {"ovr_GroupPresence_SetIsJoinable", 0x2a8f1055u, std::uint64_t{0}}})
    completion(session, optest::invoke(session, name, {argument}), type);
  completion(session, optest::invoke(session, "ovr_GroupPresence_Clear"), 0x6daa9cc3);
  provider->wait(7);
  {
    std::lock_guard lock(provider->mutex);
    CHECK(provider->states[0].at("api_name") == "menu");
    CHECK(provider->states[0].at("is_joinable") == true);
    CHECK(provider->states[5].at("api_name") == "race");
    CHECK(provider->states[5].at("lobby_session_id") == "lobby");
    CHECK(provider->states[5].at("match_session_id") == "match");
    CHECK(provider->states[5].at("deeplink_message_override") == "deeplink");
    CHECK(provider->states[5].at("is_joinable") == false);
    CHECK(provider->states[6].empty());
  }
  // Repeating the same cleared state does not create a heartbeat storm.
  completion(session, optest::invoke(session, "ovr_GroupPresence_Clear"), 0x6daa9cc3);
  Session second(optest::app(), std::make_shared<PresenceProvider>(), arena.memory());
  REQUIRE(second.initialize(optest::app().app_id) == InitResult::success);
  completion(second, optest::invoke(second, "ovr_GroupPresence_Set", {0}), 0x675f5c24, true);
  session.shutdown(); second.shutdown();
  CHECK(arena.live == 0);
}

TEST_CASE("Platform LaunchOtherApp forwards optional options and receives standalone failure through the pipe") {
  optest::Arena arena;
  Session session(optest::app(), std::make_shared<PresenceProvider>(), arena.memory());
  REQUIRE(session.initialize(optest::app().app_id) == InitResult::success);
  auto [parent, child] = venture::PipeChannel::pair();
  session.attach_launcher(child);
  unsigned calls = 0;
  std::jthread launcher([parent, &calls](std::stop_token stop) {
    parent->serve([&calls](const venture::Json& request) -> venture::Json {
      auto d = venture::read_launch_details(request.at("details"));
      CHECK(d.type == 4);
      CHECK(request.at("app") == "123");
      if (!calls++) { CHECK(d.deeplink.empty()); return {{"success", false}}; }
      CHECK(d.deeplink == "private message");
      CHECK(d.destination == "race"); CHECK(d.lobby == "lobby"); CHECK(d.match == "match");
      return {{"success", true}};
    }, stop);
  });
  auto id = optest::invoke(session, "ovr_Application_LaunchOtherApp", {123, 0});
  completion(session, id, 0x54e2d1f8, true, "Not supported on Standalone platform.");
  auto options = optest::invoke(session, "ovr_ApplicationOptions_Create");
  optest::invoke(session, "ovr_ApplicationOptions_SetDeeplinkMessage", {options, venture::h2g("private message")});
  optest::invoke(session, "ovr_ApplicationOptions_SetDestinationApiName", {options, venture::h2g("race")});
  optest::invoke(session, "ovr_ApplicationOptions_SetLobbySessionId", {options, venture::h2g("lobby")});
  optest::invoke(session, "ovr_ApplicationOptions_SetMatchSessionId", {options, venture::h2g("match")});
  id = optest::invoke(session, "ovr_Application_LaunchOtherApp", {123, options});
  optest::invoke(session, "ovr_ApplicationOptions_Destroy", {options});
  completion(session, id, 0x54e2d1f8);
  session.shutdown(); child->close(); launcher.join();
}
