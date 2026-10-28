#include "common/environment.h"
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <set>
#include <nlohmann/json.hpp>

#include "common/log.h"
#include "common/paths.h"
#include "ovrplatform_support.h"
#include "hle/ovrplatform/diagnostics.h"
#include "hle/ovrplatform/progression.h"
#include "hle/ovrplatform/identity_iap.h"

#include "hle/ovrplatform/social.h"
#ifdef _WIN32
#include "common/file_ops.h"
#include "common/meta/accounts.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "../async_wait.h"

using namespace venture::ovrplatform;
namespace optest = venture::ovrplatform::test;
namespace {
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalid = INVALID_SOCKET;
void close_socket(Socket s) { closesocket(s); }
#else
using Socket = int;
constexpr Socket invalid = -1;
void close_socket(Socket s) { close(s); }
#endif
// A real loopback HTTP server exercises libcurl framing/status handling. All
// credentials and identities here are synthetic; no SecretStore is opened.
class Server {
 public:
  Server() {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data))
      throw std::runtime_error("WSAStartup");
#endif
    socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_ == invalid) throw std::runtime_error("test socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)))
      throw std::runtime_error("test bind");
#ifdef _WIN32
    int size = sizeof(address);
#else
    socklen_t size = sizeof(address);
#endif
    if (getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &size))
      throw std::runtime_error("test getsockname");
    port = ntohs(address.sin_port);
    if (listen(socket_, 4)) throw std::runtime_error("test listen");
    worker_ = std::jthread([this](std::stop_token stop) { serve(stop); });
  }
  ~Server() {
    worker_.request_stop();
    worker_.join();
    close_socket(socket_);
#ifdef _WIN32
    WSACleanup();
#endif
  }
  void push(std::string body, int status = 200) {
    std::lock_guard lock(mutex_);
    responses_.emplace_back(status, std::move(body));
  }
  std::vector<std::string> requests() {
    std::lock_guard lock(mutex_);
    return requests_;
  }
  std::uint16_t port = 0;

 private:
  void serve(std::stop_token stop) {
    while (!stop.stop_requested()) {
      fd_set set;
      FD_ZERO(&set);
      FD_SET(socket_, &set);
      timeval timeout{0, 100000};
      if (select(static_cast<int>(socket_ + 1), &set, nullptr, nullptr,
                 &timeout) <= 0)
        continue;
      Socket client = accept(socket_, nullptr, nullptr);
      if (client == invalid) continue;
      std::string request;
      char buffer[4096];
      while (!stop.stop_requested()) {
        fd_set readers;
        FD_ZERO(&readers);
        FD_SET(client, &readers);
        timeval read_timeout{0, 100000};
        if (select(static_cast<int>(client + 1), &readers, nullptr, nullptr,
                   &read_timeout) <= 0)
          continue;
        auto n = recv(client, buffer, sizeof(buffer), 0);
        if (n <= 0) break;
        request.append(buffer, n);
        auto end = request.find("\r\n\r\n");
        if (end == std::string::npos) continue;
        std::size_t length = 0;
        auto at = request.find("Content-Length: ");
        if (at != std::string::npos)
          length = std::stoul(request.substr(at + 16));
        if (request.size() >= end + 4 + length) break;
      }
      std::pair<int, std::string> response{500, "{}"};
      {
        std::lock_guard lock(mutex_);
        requests_.push_back(request);
        if (!responses_.empty()) {
          response = std::move(responses_.front());
          responses_.pop_front();
        }
      }
      if (response.first == 0) {  // status 0: drop the connection without a response
        close_socket(client);
        continue;
      }
      auto bytes =
          "HTTP/1.1 " + std::to_string(response.first) +
          " Test\r\nContent-Type: application/json\r\nContent-Length: " +
          std::to_string(response.second.size()) +
          "\r\nConnection: close\r\n\r\n" + response.second;
      std::size_t sent = 0;
      while (sent < bytes.size()) {
        auto n = send(client, bytes.data() + sent,
                      static_cast<int>(bytes.size() - sent), 0);
        if (n <= 0) break;
        sent += n;
      }
      close_socket(client);
    }
  }
  Socket socket_ = invalid;
  std::mutex mutex_;
  std::deque<std::pair<int, std::string>> responses_;
  std::vector<std::string> requests_;
  std::jthread worker_;
};
using Json = nlohmann::json;
Json library(bool entitled = true, bool complete = true) {
  auto nodes = Json::array();
  if (entitled)
    nodes.push_back(
        {{"item",
          {{"id", optest::app().app_id}, {"platform", "ANDROID_6DOF"}}}});
  return {{"data",
           {{"viewer",
             {{"user",
               {{"id", "42"},
                {"alias", "root-alias-never-exposed"},
                {"active_entitlements",
                 {{"nodes", nodes},
                  {"page_info", {{"has_next_page", !complete}}}}}}}}}}}};
}
}  // namespace

TEST_CASE("Platform HTTP v2 uses pinned graph shapes, numeric templates and allowed parameters") {
  Server server;
  auto http = loopback_http_transport(server.port);
  http->register_endpoint({"fixture-read", HttpMethod::get, "/{app}/{id}", {"fields"}});
  http->register_endpoint({"fixture-write", HttpMethod::post, "/fixture", {"value"}});
  http->register_endpoint({"fixture-delete", HttpMethod::erase, "/fixture/{id}", {"value"}});
  CHECK_THROWS(http->register_endpoint({"fixture-write", HttpMethod::post, "/duplicate", {}}));
  CHECK_THROWS(http->register_endpoint({"bad", HttpMethod::get, "/{unknown}", {}}));
  server.push("{}");
  server.push("{}");
  server.push("{}");
  CHECK_FALSE(
      http->send({"fixture-read", {{"fields", "id,alias"}}, "fixture-bearer", "123", "456"}, {})
          .error);
  CHECK_FALSE(http->send({"fixture-write", {{"value", "a b&?"}}, "fixture-bearer"}, {}).error);
  CHECK_FALSE(
      http->send({"fixture-delete", {{"value", "true"}}, "fixture-bearer", {}, "456"}, {}).error);
  auto requests = server.requests();
  REQUIRE(requests.size() == 3);
  CHECK(requests[0].starts_with("GET /v1.20/123/456?fields=id%2Calias&app_version=0 HTTP/1.1"));
  CHECK(requests[1].starts_with("POST /v1.20/fixture?value=a%20b%26%3F&app_version=0 HTTP/1.1"));
  CHECK(requests[2].starts_with("DELETE /v1.20/fixture/456?value=true&app_version=0 HTTP/1.1"));
  for (auto& request : requests) {
    CHECK(request.find("Authorization: Bearer fixture-bearer\r\n") != std::string::npos);
    CHECK(request.substr(request.find("\r\n\r\n") + 4).empty());
    CHECK(request.find("access_token=") == std::string::npos);
  }
  CHECK(http->send({"fixture-read", {}, {}, "123", "0"}, {}).error.code ==
        ErrorCode::invalid_argument);
  CHECK(http->send({"fixture-read", {{"secret", "value"}}, {}, "123", "456"}, {}).error.code ==
        ErrorCode::invalid_argument);
  CHECK(http->send({"fixture-write", {}, "bad\r\nheader"}, {}).error.code ==
        ErrorCode::invalid_argument);
  CHECK(http->send({"not-registered"}, {}).error.code == ErrorCode::invalid_argument);
  CHECK(server.requests().size() == 3);
}

namespace {
struct ProgressionFixture {
  Server server;
  std::shared_ptr<HttpTransport> http = loopback_http_transport(server.port);
  std::shared_ptr<optest::FakeStore> store = std::make_shared<optest::FakeStore>();
  std::shared_ptr<Provider> provider = owned_library_provider(optest::app(), store, http);
  optest::Arena arena;
  Session session{optest::app(), provider, arena.memory()};
  ProgressionFixture() {
    register_progression_endpoints(*http);
    register_progression_handlers(session, {true, true, true, true});
    REQUIRE(session.initialize(optest::app().app_id) == InitResult::success);
    server.push(R"({"access_token":"fixture-app-token"})");
    server.push(R"({"id":"123456"})");
    REQUIRE_FALSE(provider->logged_in_user({}).error);
  }
  Handle call(std::string_view name, std::string response,
              std::initializer_list<std::uint64_t> args = {}, int status = 200) {
    server.push(std::move(response), status);
    auto request = optest::invoke(session, name, args);
    auto message = optest::wait_pop(session);
    CHECK(session.get(message, "ovr_Message_GetRequestID") == request);
    CHECK(session.get(message, "ovr_Message_GetType") == optest::spec(name).type);
    return message;
  }
  Handle success(std::string_view name, std::string response,
                 std::initializer_list<std::uint64_t> args = {}) {
    auto message = call(name, std::move(response), args);
    CHECK(session.get(message, "ovr_Message_IsError") == 0);
    return message;
  }
  std::string last() { return server.requests().back(); }
};
constexpr const char* empty_entries = R"({"data":[],"summary":{"total_count":0}})";
constexpr const char* entry_response = R"({"data":[{"id":"888","user":{"id":"999","alias":"fixture-alias","display_name":"Fixture name","presence_status":"ONLINE","profile_url":"https://example.test/picture"},"rank":2,"score":"-9223372036854775808","timestamp":1720000000,"display_score":"1:23.456","extra_data_base64":"AP8BAA==","supplementary_metric":{"id":"777","metric":-5}}],"summary":{"total_count":"12345678901"},"paging":{"next":"https://graph.oculus.com/v1.20/leaderboard_entries?after=next","previous":"https://graph.oculus.com/v1.20/leaderboard_entries?before=prev"}})";
}

TEST_CASE("Platform progression covers all leaderboard request shapes and borrowed models") {
  ProgressionFixture f;
  const char* name = "race & track";
  auto message = f.success("ovr_Leaderboard_Get", R"({"data":[{"api_name":"race","id":"555","destination":{"api_name":"track","display_name":"Track","deeplink_message":"race=1","shareable_uri":"https://example.test/track"}}],"paging":{"next":"https://graph.oculus.com/v1.20/3927222113966481/leaderboards?after=x"}})", {venture::h2g(name)});
  auto array = f.session.get(message, "ovr_Message_GetLeaderboardArray");
  auto board = f.session.get(array, "ovr_LeaderboardArray_GetElement", 0);
  CHECK(f.session.get(board, "ovr_Leaderboard_GetID") == 555);
  auto dest = f.session.get(board, "ovr_Leaderboard_GetDestination");
  CHECK(std::string(venture::g2h<const char>(f.session.get(dest, "ovr_Destination_GetApiName"))) == "track");
  CHECK(f.last().starts_with("GET /v1.20/3927222113966481/leaderboards?api_name=race%20%26%20track&fields="));
  auto page = f.success("ovr_Leaderboard_GetNextLeaderboardArrayPage", R"({"data":[]})", {array});
  f.session.free_message(page); f.session.free_message(message);
  message = f.success("ovr_Leaderboard_GetEntriesAfterRank", empty_entries, {venture::h2g(name), 5, UINT64_MAX});
  CHECK(f.last().find("start_at=OFFSET&offset=18446744073709551615") != std::string::npos);
  f.session.free_message(message);
  Id ids[] = {12, 34};
  message = f.success("ovr_Leaderboard_GetEntriesByIds", entry_response, {venture::h2g(name), 5, 1, venture::h2g(ids), 2});
  CHECK(f.last().find("filter=USER_IDS&user_ids=%5B12%2C34%5D&start_at=CENTERED_ON_VIEWER") != std::string::npos);
  array = f.session.get(message, "ovr_Message_GetLeaderboardEntryArray");
  page = f.success("ovr_Leaderboard_GetPreviousEntries", empty_entries, {array});
  CHECK(f.last().starts_with("GET /v1.20/leaderboard_entries?before=prev&app_version=0 "));
  f.session.free_message(page); f.session.free_message(message);
  const unsigned char bytes[] = {0, 255, 1, 0};
  message = f.success("ovr_Leaderboard_WriteEntry", R"({"did_update":true,"updated_challenge_ids":["44","18446744073709551615"]})",
                      {venture::h2g(name), static_cast<std::uint64_t>(INT64_MIN), venture::h2g(bytes), 4, 0});
  auto update = f.session.get(message, "ovr_Message_GetLeaderboardUpdateStatus");
  CHECK(f.session.get(update, "ovr_LeaderboardUpdateStatus_GetDidUpdate") == 1);
  CHECK(f.session.get(update, "ovr_LeaderboardUpdateStatus_GetUpdatedChallengeIdsSize") == 2);
  CHECK(optest::invoke(f.session, "ovr_LeaderboardUpdateStatus_GetUpdatedChallengeId", {update, 1}) == UINT64_MAX);
  CHECK(f.last().starts_with("POST /v1.20/leaderboard_submit_entry?api_name="));
  CHECK(f.last().find("score=-9223372036854775808&extra_data_base64=AP8BAA%3D%3D") != std::string::npos);
  CHECK(f.last().find("force_update") == std::string::npos);
  f.session.free_message(message);
  message = f.success("ovr_Leaderboard_WriteEntryWithSupplementaryMetric", R"({"did_update":false})",
                      {venture::h2g(name), 123, static_cast<std::uint64_t>(-7), 0, 0, 1});
  CHECK(f.last().find("supplementary_metric=-7&force_update=true") != std::string::npos);
  f.session.free_message(message);
  for (std::size_t i = 2; i < f.server.requests().size(); ++i) {
    auto request = f.server.requests()[i];
    CHECK(request.find("Authorization: Bearer fixture-app-token\r\n") != std::string::npos);
    CHECK(request.substr(request.find("\r\n\r\n") + 4).empty());
    CHECK(request.find("app_version=0") != std::string::npos);
  }
  CHECK(f.arena.live == 0);
}

TEST_CASE("Platform progression implements all achievement calls and pagination") {
  ProgressionFixture f;
  auto message = f.success("ovr_Achievements_GetAllDefinitions", R"({"data":[{"api_name":"laps","achievement_type":"COUNT","target":"18446744073709551615","bitfield_length":0},{"api_name":"bits","achievement_type":"BITFIELD","target":"2","bitfield_length":8}],"paging":{"next":"https://graph.oculus.com/v1.20/application/achievement_definitions?after=x"}})");
  auto array = f.session.get(message, "ovr_Message_GetAchievementDefinitionArray");
  CHECK(f.session.get(array, "ovr_AchievementDefinitionArray_GetSize") == 2);
  auto definition = optest::invoke(f.session, "ovr_AchievementDefinitionArray_GetElement", {array, 0});
  CHECK(f.session.get(definition, "ovr_AchievementDefinition_GetTarget") == UINT64_MAX);
  CHECK(f.session.get(definition, "ovr_AchievementDefinition_GetType") == 3);
  definition = optest::invoke(f.session, "ovr_AchievementDefinitionArray_GetElement", {array, 1});
  CHECK(f.session.get(definition, "ovr_AchievementDefinition_GetBitfieldLength") == 8);
  auto page = f.success("ovr_Achievements_GetNextAchievementDefinitionArrayPage", R"({"data":[]})", {array});
  f.session.free_message(page); f.session.free_message(message);
  const char* names[] = {"laps", "bits"};
  message = f.success("ovr_Achievements_GetDefinitionsByName", R"({"data":[]})", {venture::h2g(names), 2});
  CHECK(f.last().find("api_names=%5B%22laps%22%2C%22bits%22%5D") != std::string::npos);
  f.session.free_message(message);
  message = f.success("ovr_Achievements_GetAllProgress", R"({"data":[{"definition":{"api_name":"laps"},"count_progress":"18446744073709551615","bitfield_progress":"1010","is_unlocked":true,"unlock_time":1720000000}],"paging":{"next":"https://graph.oculus.com/v1.20/me/achievements?after=x"}})");
  array = f.session.get(message, "ovr_Message_GetAchievementProgressArray");
  auto progress = f.session.get(array, "ovr_AchievementProgressArray_GetElement", 0);
  CHECK(f.session.get(progress, "ovr_AchievementProgress_GetCount") == UINT64_MAX);
  CHECK(f.session.get(progress, "ovr_AchievementProgress_GetIsUnlocked") == 1);
  CHECK(f.session.get(progress, "ovr_AchievementProgress_GetUnlockTime") == 1720000000);
  CHECK(std::string(venture::g2h<const char>(f.session.get(progress, "ovr_AchievementProgress_GetBitfield"))) == "1010");
  page = f.success("ovr_Achievements_GetNextAchievementProgressArrayPage", R"({"data":[]})", {array});
  f.session.free_message(page); f.session.free_message(message);
  message = f.success("ovr_Achievements_GetProgressByName", R"({"data":[]})", {venture::h2g(names), 2});
  f.session.free_message(message);
  message = f.success("ovr_Achievements_Unlock", R"({"api_name":"laps","just_unlocked":true})", {venture::h2g(names[0])});
  auto update = f.session.get(message, "ovr_Message_GetAchievementUpdate");
  CHECK(f.session.get(update, "ovr_AchievementUpdate_GetJustUnlocked") == 1);
  CHECK(std::string(venture::g2h<const char>(f.session.get(update, "ovr_AchievementUpdate_GetName"))) == "laps");
  CHECK(f.last().find("api_name=laps&force_unlock=true") != std::string::npos);
  f.session.free_message(message);
  message = f.success("ovr_Achievements_AddCount", R"({"api_name":"laps","just_unlocked":false})", {venture::h2g(names[0]), UINT64_MAX});
  CHECK(f.last().find("add_count=9223372036854775807") != std::string::npos);
  f.session.free_message(message);
  const char* bits = "1001";
  message = f.success("ovr_Achievements_AddFields", R"({"api_name":"bits","just_unlocked":false})", {venture::h2g(names[1]), venture::h2g(bits)});
  CHECK(f.last().find("add_bits=1001") != std::string::npos);
  f.session.free_message(message);
  CHECK(f.arena.live == 0);
}

TEST_CASE("Platform paging never attaches credentials to an untrusted URL") {
  Server server;
  auto http = loopback_http_transport(server.port);
  http->register_endpoint({"fixture-page", HttpMethod::get, "/fixture", {}});
  for (auto url :
       {"http://graph.oculus.com/v1.20/fixture", "https://graph.oculus.com.evil/v1.20/fixture",
        "https://graph.oculus.com@evil/v1.20/fixture",
        "https://user@graph.oculus.com/v1.20/fixture", "https://graph.oculus.com:444/v1.20/fixture",
        "https://graph.oculus.com/me", "https://graph.oculus.com/v1.20/../me",
        "https://graph.oculus.com/v1.20/%2e%2e/me",
        "https://graph.oculus.com/v1.20/fixture#fragment",
        "https://graph.oculus.com/v1.20/fixture\r\nHeader: bad"}) {
    CHECK_FALSE(valid_next_page_url(url));
    CHECK(http->send({"fixture-page", {}, "fixture-bearer", {}, {}, url}, {}).error.code ==
          ErrorCode::invalid_argument);
  }
  CHECK(server.requests().empty());
  server.push("{}");
  CHECK_FALSE(http->send({"fixture-page",
                          {},
                          "fixture-bearer",
                          {},
                          {},
                          "https://graph.oculus.com:443/v1.20/fixture?after=a%2Fb&app_version=0"},
                         {})
                  .error);
  REQUIRE(server.requests().size() == 1);
  CHECK(server.requests()[0].starts_with("GET /v1.20/fixture?after=a%2Fb&app_version=0 HTTP/1.1"));
  CHECK(server.requests()[0].find("Authorization: Bearer fixture-bearer") != std::string::npos);
  server.push("{}");
  CHECK_FALSE(http->send({"fixture-page",
                          {},
                          "fixture-bearer",
                          {},
                          {},
                          "https://graph.oculus.com/v1.20/fixture?after=cursor"},
                         {})
                  .error);
  CHECK(server.requests()[1].starts_with("GET /v1.20/fixture?after=cursor&app_version=0 HTTP/1.1"));
}

TEST_CASE("Platform read backoff is bounded and writes are never retried") {
  Server server;
  auto http = loopback_http_transport(server.port);
  http->register_endpoint({"read", HttpMethod::get, "/fixture", {}});
  http->register_endpoint({"write", HttpMethod::post, "/fixture", {}});
  server.push("{}", 429);
  server.push("{}", 503);
  server.push("{}");
  auto start = std::chrono::steady_clock::now();
  CHECK_FALSE(http->send({"read"}, {}).error);
  CHECK(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(700));
  CHECK(server.requests().size() == 3);
  server.push("{}", 503);
  CHECK(http->send({"write"}, {}).error.code == ErrorCode::unavailable);
  CHECK(server.requests().size() == 4);
  std::stop_source stop;
  stop.request_stop();
  CHECK(http->send({"write"}, stop.get_token()).error.code == ErrorCode::cancelled);
  CHECK(server.requests().size() == 4);
}

TEST_CASE(
    "Platform entitlement never fails open on partial, wrong-scope or "
    "malformed HTTP responses") {
  Server server;
  auto store = std::make_shared<optest::FakeStore>();
  auto provider = owned_library_provider(optest::app(), store,
                                         loopback_http_transport(server.port));
  REQUIRE_FALSE(provider->prepare());
  auto body = library();
  ErrorCode expected = ErrorCode::invalid_response;
  int status = 200;
  SUBCASE("incomplete without the app") {
    body = library(false, false);
    expected = ErrorCode::incomplete;
  }
  SUBCASE("missing pagination") {
    body = library(false);
    body["data"]["viewer"]["user"]["active_entitlements"].erase("page_info");
    expected = ErrorCode::incomplete;
  }
  SUBCASE("wrong account") {
    body["data"]["viewer"]["user"]["id"] = "99";
    expected = ErrorCode::identity_mismatch;
  }
  SUBCASE("Rift entitlement") {
    body["data"]["viewer"]["user"]["active_entitlements"]["nodes"][0]["item"]
        ["platform"] = "PC";
    expected = ErrorCode::identity_mismatch;
  }
  SUBCASE("generic Android without Quest evidence") {
    body["data"]["viewer"]["user"]["active_entitlements"]["nodes"][0]["item"]
        ["platform"] = "ANDROID";
    expected = ErrorCode::identity_mismatch;
  }
  SUBCASE("wrong application") {
    body["data"]["viewer"]["user"]["active_entitlements"]["nodes"][0]["item"]
        ["id"] = "999";
    expected = ErrorCode::not_entitled;
  }
  SUBCASE("Graph error") {
    body = {{"error", {{"message", "secret-must-not-escape"}, {"code", 190}}}};
    expected = ErrorCode::unauthenticated;
  }
  SUBCASE("HTTP expired") {
    status = 401;
    expected = ErrorCode::unauthenticated;
  }
  SUBCASE("HTTP unavailable") {
    status = 503;
    expected = ErrorCode::unavailable;
  }
  SUBCASE("redirects are not followed with credentials") {
    status = 302;
    expected = ErrorCode::unavailable;
  }
  SUBCASE("malformed schema") { body = {{"data", nullptr}}; }
  server.push(body.dump(), status);
  auto result = provider->entitlement({});
  CHECK(result.error.code == expected);
  CHECK(result.error.message.find("secret-must-not-escape") ==
        std::string::npos);
}

TEST_CASE(
    "Platform discards HTTP success if the account changes during the "
    "transfer") {
  Server server;
  server.push(library().dump());
  auto store = std::make_shared<optest::FakeStore>();
  class ChangeAfterResponse final : public HttpTransport {
   public:
    std::shared_ptr<optest::FakeStore> store;
    std::shared_ptr<HttpTransport> inner;
    HttpResponse post(std::string_view path, const Fields& fields,
                      std::stop_token stop) override {
      auto response = inner->post(path, fields, stop);
      store->replace();
      return response;
    }
    HttpResponse get(std::string_view path, const Fields& fields,
                     std::stop_token stop) override {
      return inner->get(path, fields, stop);
    }
  };
  auto transport = std::make_shared<ChangeAfterResponse>();
  transport->store = store;
  transport->inner = loopback_http_transport(server.port);
  auto provider = owned_library_provider(optest::app(), store, transport);
  REQUIRE_FALSE(provider->prepare());
  CHECK(provider->entitlement({}).error.code == ErrorCode::identity_mismatch);
  CHECK(server.requests().size() == 1);
}

TEST_CASE("Platform user proofs use fresh Meta nonces and the asynchronous A64 message ABI") {
  Server server;
  server.push(R"({"access_token":"fixture-app-token"})");
  server.push(R"({"id":"123456"})");
  server.push(R"({"nonce":"fixture-single-use-first"})");
  server.push(R"({"nonce":"fixture-single-use-second"})");
  auto store = std::make_shared<optest::FakeStore>();
  auto provider = owned_library_provider(optest::app(), store, loopback_http_transport(server.port));
  optest::Arena arena;
  Session session(optest::app(), provider, arena.memory());
  REQUIRE(session.initialize(optest::app().app_id) == InitResult::success);
  const auto first = optest::invoke(session, "ovr_User_GetUserProof");
  const auto second = optest::invoke(session, "ovr_User_GetUserProof");
  REQUIRE(first != 0);
  CHECK(second > first);
  const auto m1 = optest::wait_pop(session);
  const auto m2 = optest::wait_pop(session);
  const auto id1 = optest::invoke(session, "ovr_Message_GetRequestID", {m1});
  const auto id2 = optest::invoke(session, "ovr_Message_GetRequestID", {m2});
  CHECK(((id1 == first && id2 == second) || (id1 == second && id2 == first)));
  CHECK(optest::invoke(session, "ovr_Message_GetType", {m1}) == 0x22810483);
  REQUIRE(optest::invoke(session, "ovr_Message_IsError", {m1}) == 0);
  REQUIRE(optest::invoke(session, "ovr_Message_IsError", {m2}) == 0);
  const auto proof1 = optest::invoke(session, "ovr_Message_GetUserProof", {m1});
  const auto proof2 = optest::invoke(session, "ovr_Message_GetUserProof", {m2});
  REQUIRE(proof1 != 0);
  REQUIRE(proof2 != 0);
  const auto nonce1 = optest::invoke(session, "ovr_UserProof_GetNonce", {proof1});
  const auto nonce2 = optest::invoke(session, "ovr_UserProof_GetNonce", {proof2});
  const std::set<std::string> nonces{venture::g2h<char>(nonce1), venture::g2h<char>(nonce2)};
  CHECK(nonces == std::set<std::string>{"fixture-single-use-first", "fixture-single-use-second"});
  CHECK(optest::invoke(session, "ovr_Message_GetString", {m1}) == 0);
  optest::invoke(session, "ovr_FreeMessage", {m1});
  CHECK(optest::invoke(session, "ovr_UserProof_GetNonce", {proof1}) == 0);
  CHECK(nonces.contains(venture::g2h<char>(nonce2)));
  optest::invoke(session, "ovr_FreeMessage", {m2});
  CHECK(arena.live == 0);
  CHECK(session.pop() == 0);
  const auto requests = server.requests();
  REQUIRE(requests.size() == 4);
  for (std::size_t i : {2, 3}) {
    CHECK(requests[i].starts_with("POST /user_nonce_generate HTTP/1.1"));
    CHECK(requests[i].substr(requests[i].find("\r\n\r\n") + 4) == "access_token=fixture-app-token");
    CHECK(requests[i].find(store->value.access_token) == std::string::npos);
  }
}

TEST_CASE("Platform user proof HTTP and malformed responses fail without exposing secrets") {
  Server server;
  server.push(R"({"access_token":"fixture-app-token"})");
  server.push(R"({"id":"123456"})");
  int status = 200;
  std::string body;
  auto expected = ErrorCode::invalid_response;
  SUBCASE("HTTP denied") {
    status = 403; expected = ErrorCode::unauthenticated;
    body = R"({"error":{"message":"fixture-app-token fixture-private-nonce"}})";
  }
  SUBCASE("HTTP unavailable") {
    status = 503; expected = ErrorCode::unavailable;
    body = "fixture-app-token fixture-private-nonce";
  }
  SUBCASE("HTTP redirect status is not a proof") {
    status = 302; expected = ErrorCode::unavailable; body = "{}";
  }
  SUBCASE("Graph error in HTTP 200") {
    expected = ErrorCode::unavailable;
    body = R"({"error":{"message":"fixture-app-token"},"nonce":"fixture-private-nonce"})";
  }
  SUBCASE("invalid JSON") { body = "fixture-private-nonce"; }
  SUBCASE("non-object") { body = R"(["fixture-private-nonce"])"; }
  SUBCASE("missing nonce") { body = "{}"; }
  SUBCASE("empty nonce") { body = R"({"nonce":""})"; }
  SUBCASE("numeric nonce is not coerced") { body = R"({"nonce":123})"; }
  SUBCASE("null nonce") { body = R"({"nonce":null})"; }
  SUBCASE("embedded NUL cannot cross the C string ABI") { body = R"({"nonce":"fixture\u0000private-nonce"})"; }
  server.push(body, status);
  auto provider = owned_library_provider(optest::app(), std::make_shared<optest::FakeStore>(),
                                         loopback_http_transport(server.port));
  optest::Arena arena;
  Session session(optest::app(), provider, arena.memory());
  REQUIRE(session.initialize(optest::app().app_id) == InitResult::success);
  const auto request = optest::invoke(session, "ovr_User_GetUserProof");
  const auto message = optest::wait_pop(session);
  CHECK(session.get(message, "ovr_Message_GetRequestID") == request);
  CHECK(session.get(message, "ovr_Message_GetType") == 0x22810483);
  CHECK(session.get(message, "ovr_Message_IsError") == 1);
  CHECK(session.get(message, "ovr_Message_GetUserProof") == 0);
  const auto error = session.get(message, "ovr_Message_GetError");
  CHECK(static_cast<std::int32_t>(session.get(error, "ovr_Error_GetCode")) == static_cast<std::int32_t>(expected));
  CHECK(session.get(error, "ovr_Error_GetHttpCode") == status);
  const std::string text(venture::g2h<char>(session.get(error, "ovr_Error_GetMessage")));
  CHECK(text == failure(expected).message);
  session.free_message(message);
  CHECK(arena.live == 0);
  CHECK(server.requests().size() == 3); // No automatic retry, even on 503.
}

namespace {
class ProofTransport : public HttpTransport {
 public:
  std::function<void()> during_proof = [] {};
  HttpResponse proof{200, R"({"nonce":"fixture-private-nonce"})", {}};
  unsigned proofs = 0;
  HttpResponse post(std::string_view path, const Fields&, std::stop_token) override {
    if (path == "/authenticate_application")
      return {200, R"({"access_token":"fixture-app-token"})", {}};
    REQUIRE(path == "/user_nonce_generate");
    ++proofs;
    during_proof();
    return proof;
  }
  HttpResponse get(std::string_view path, const Fields&, std::stop_token) override {
    REQUIRE(path == "/me");
    return {200, R"({"id":"123456"})", {}};
  }
};
class ProgressionTransport final : public ProofTransport {
 public:
  std::function<void()> during_send = [] {};
  unsigned sends = 0;
  HttpResponse send(const HttpRequest& request, std::stop_token) override {
    CHECK(request.bearer == "fixture-app-token");
    CHECK(request.app_id == optest::app().app_id);
    ++sends;
    during_send();
    return {200, R"({"data":[]})", {}};
  }
};
}

TEST_CASE(
    "Platform accounts keep entitlement and identity credentials separate") {
  Server server;
  auto identity = std::make_shared<optest::FakeStore>();
  auto owner = std::make_shared<optest::FakeStore>();
  owner->value = {"fake-owner-token", "84"};
  auto body = library();
  body["data"]["viewer"]["user"]["id"] = "84";
  server.push(body.dump());
  server.push(R"({"access_token":"fake-app-only-token"})");
  server.push(R"({"id":"123456","alias":"fixture-app-alias"})");
  auto provider =
      owned_library_provider(optest::app(), LaunchCredentials{identity, owner},
                             loopback_http_transport(server.port));
  REQUIRE_FALSE(provider->prepare());
  CHECK_FALSE(provider->entitlement({}).error);
  CHECK(provider->access_token({}).text == "fake-app-only-token");
  auto requests = server.requests();
  REQUIRE(requests.size() == 3);
  CHECK(requests[0].find("fake-owner-token") != std::string::npos);
  CHECK(requests[1].find(identity->value.access_token) != std::string::npos);
  owner->replace();
  CHECK(provider->entitlement({}).error.code == ErrorCode::identity_mismatch);
  CHECK_FALSE(provider->logged_in_user({}).error);
  server.push(R"({"nonce":"fixture-identity-nonce"})");
  CHECK(provider->user_proof({}).text == "fixture-identity-nonce");
  requests = server.requests();
  REQUIRE(requests.size() == 4);
  CHECK(requests[3].find("fake-app-only-token") != std::string::npos);
  CHECK(requests[3].find("fake-owner-token") == std::string::npos);
  identity->replace();
  CHECK(provider->logged_in_user({}).error.code ==
        ErrorCode::identity_mismatch);
  CHECK(provider->user_proof({}).error.code == ErrorCode::identity_mismatch);
  CHECK(server.requests().size() == 4);
}

#ifdef _WIN32
#endif

namespace {
std::shared_ptr<Provider> service_provider(
    Server& server, std::shared_ptr<SecretStore> store =
                        std::make_shared<optest::FakeStore>()) {
  auto http = loopback_http_transport(server.port);
  register_identity_iap_endpoints(*http);
  auto p = owned_library_provider(optest::app(), std::move(store), http);
  REQUIRE_FALSE(p->prepare());
  server.push(R"({"access_token":"fixture-app-token"})");
  server.push(R"({"id":"123456"})");
  REQUIRE_FALSE(p->access_token({}).error);
  return p;
}
Json product_fixture(std::string sku = "fixture-sku") {
  return {{"sku", sku},
          {"type", "SUBSCRIPTION"},
          {"current_offer",
           {{"name", "Fixture product"},
            {"description", "Fixture description"},
            {"price",
             {{"currency", "USD"},
              {"amount_in_hundredths", 199},
              {"formatted", "$1.99"}}}}},
          {"content_rating",
           {{"age_rating_text", "Everyone"},
            {"descriptors", {"First", "Second"}},
            {"interactive_elements", {"In-App Purchases"}}}},
          {"billing_plans",
           {{"data",
             Json::array(
                 {{{"paid_offer",
                    {{"price",
                      {{"currency", "USD"},
                       {"amount_in_hundredths", 199},
                       {"formatted", "$1.99"}}},
                     {"subscription_term", "MONTHLY"}}},
                   {"trial_offers",
                    {{"data", Json::array({{{"price",
                                             {{"currency", "USD"},
                                              {"amount_in_hundredths", 0},
                                              {"formatted", "$0.00"}}},
                                            {"trial_term", "WEEKLY"},
                                            {"trial_type", "FREE_TRIAL"},
                                            {"max_term_count", 1}}})}}}}})}}}};
}
Json purchase_fixture(std::string sku, std::string type) {
  return {{"id", "18446744073709551615"},
          {"grant_time", 1700000000},
          {"expiration_time", nullptr},
          {"item", {{"sku", std::move(sku)}, {"type", std::move(type)}}}};
}
}  // namespace

TEST_CASE(
    "Platform IAP catalog models and next page survive message ownership and "
    "indexed getters") {
  Server server;
  auto p = service_provider(server);
  optest::Arena arena;
  Session session(optest::app(), p, arena.memory());
  REQUIRE(session.initialize(optest::app().app_id) == InitResult::success);
  const auto next =
      "https://graph.oculus.com/v1.20/application/"
      "available_purchases?after=fixture";
  server.push(Json{
      {"data", Json::array({product_fixture(), product_fixture("second")})},
      {"paging", {{"next", next}}}}
                  .dump());
  const char* skus[] = {"fixture-sku", nullptr, "second"};
  optest::invoke(session, "ovr_IAP_GetProductsBySKU", {venture::h2g(skus), 3});
  auto m = optest::wait_pop(session);
  REQUIRE(session.get(m, "ovr_Message_IsError") == 0);
  const auto native = optest::invoke(session, "ovr_Message_GetNativeMessage", {m});
  REQUIRE(native == m);
  auto a = session.get(native, "ovr_Message_GetProductArray");
  CHECK(optest::invoke(session, "ovr_ProductArray_GetSize", {a}) == 2);
  CHECK(optest::invoke(session, "ovr_ProductArray_HasNextPage", {a}) == 1);
  auto second = optest::invoke(session, "ovr_ProductArray_GetElement", {a, 1});
  CHECK(std::string(venture::g2h<char>(session.get(second, "ovr_Product_GetSKU"))) ==
        "second");
  CHECK(session.get(second, "ovr_Product_GetType") == 3);
  auto price = session.get(second, "ovr_Product_GetPrice");
  CHECK(session.get(price, "ovr_Price_GetAmountInHundredths") == 199);
  auto rating = session.get(second, "ovr_Product_GetContentRating");
  CHECK(optest::invoke(session, "ovr_ContentRating_GetDescriptorsSize",
                       {rating}) == 2);
  CHECK(std::string(venture::g2h<char>(
            optest::invoke(session, "ovr_ContentRating_GetDescriptor",
                           {rating, 0xffffffff00000001ull}))) == "Second");
  CHECK(session.get(rating, "ovr_ContentRating_GetDescriptor", 2) == 0);
  auto plans = session.get(second, "ovr_Product_GetBillingPlans");
  auto plan = session.get(plans, "ovr_BillingPlanArray_GetElement", 0);
  auto paid = session.get(plan, "ovr_BillingPlan_GetPaidOffer");
  CHECK(session.get(paid, "ovr_PaidOffer_GetSubscriptionTerm") == 3);
  auto trials = session.get(plan, "ovr_BillingPlan_GetTrialOffers");
  auto trial = session.get(trials, "ovr_TrialOfferArray_GetElement", 0);
  CHECK(session.get(trial, "ovr_TrialOffer_GetTrialType") == 2);
  CHECK(session.get(trial, "ovr_TrialOffer_GetMaxTermCount") == 1);
  server.push(R"({"data":[]})");
  optest::invoke(session, "ovr_IAP_GetNextProductArrayPage", {a});
  session.free_message(
      m);  // next URL was copied before the original graph died
  auto page = optest::wait_pop(session);
  REQUIRE(session.get(page, "ovr_Message_IsError") == 0);
  CHECK(session.get(session.get(page, "ovr_Message_GetProductArray"),
                    "ovr_ProductArray_GetSize") == 0);
  session.free_message(page);
  CHECK(arena.live == 0);
  auto requests = server.requests();
  REQUIRE(requests.size() == 4);
  CHECK(requests[2].find("skus=fixture-sku%2Csecond") != std::string::npos);
  CHECK(requests[3].starts_with(
      "GET "
      "/v1.20/application/available_purchases?after=fixture&app_version=0 "));
}

TEST_CASE(
    "Platform IAP purchases parse timestamps, types and both ID "
    "representations") {
  Server server;
  auto p = service_provider(server);
  server.push(
      Json{{"data", Json::array({purchase_fixture("durable", "DURABLE")})}}
          .dump());
  auto r = p->service("ovr_IAP_GetViewerPurchases", {}, {});
  REQUIRE_FALSE(r.error);
  const auto& m = r.model.elements.at(0);
  CHECK(std::get<std::uint64_t>(m.fields.at("PurchaseID")) == UINT64_MAX);
  CHECK(std::get<std::uint64_t>(m.fields.at("ExpirationTime")) == 0);
  CHECK(std::get<std::uint64_t>(m.fields.at("GrantTime")) == 1700000000);
  CHECK(std::get<std::int32_t>(m.fields.at("Type")) == 1);
  const auto next =
      "https://graph.oculus.com/v1.20/application/"
      "viewer_purchases?after=fixture";
  server.push(R"({"data":[]})");
  CHECK_FALSE(p->service("ovr_IAP_GetNextPurchaseArrayPage",
                         {{PageSnapshot{"PurchaseArray", next, {}}}}, {})
                  .error);
  CHECK(server.requests().back().starts_with(
      "GET /v1.20/application/viewer_purchases?after=fixture&app_version=0 "));
  server.push(
      R"({"data":[{"id":"1","grant_time":-1,"item":{"sku":"x","type":"DURABLE"}}]})");
  CHECK(p->service("ovr_IAP_GetViewerPurchases", {}, {}).error.code ==
        ErrorCode::invalid_response);
}

TEST_CASE(
    "Platform IAP only consumes on request, never retries writes, and checkout "
    "completes locally") {
  Server server;
  auto p = service_provider(server);
  CHECK(p->service("ovr_IAP_LaunchCheckoutFlow", {{std::string("fixture-sku")}},
                   {})
            .error.message == "Not supported on Standalone platform.");
  CHECK(server.requests().size() == 2);
  server.push(R"({"success":true})");
  CHECK_FALSE(p->service("ovr_IAP_ConsumePurchase",
                         {{std::string("fixture & sku")}}, {})
                  .error);
  auto requests = server.requests();
  REQUIRE(requests.size() == 3);
  CHECK(requests.back().starts_with(
      "POST "
      "/v1.20/application/"
      "consume_entitlement?sku=fixture%20%26%20sku&app_version=0 "));
  CHECK(requests.back().substr(requests.back().find("\r\n\r\n") + 4).empty());
  server.push(R"({"error":{"message":"fixture-private-server-error"}})", 503);
  CHECK(
      p->service("ovr_IAP_ConsumePurchase", {{std::string("fixture-sku")}}, {})
          .error.code == ErrorCode::unavailable);
  CHECK(server.requests().size() == 4);
  server.push(R"({"success":false})");
  CHECK(
      p->service("ovr_IAP_ConsumePurchase", {{std::string("fixture-sku")}}, {})
          .error.code == ErrorCode::invalid_response);
  server.push("{}");
  CHECK_FALSE(
      p->service("ovr_IAP_ConsumePurchase", {{std::string("fixture-sku")}}, {})
          .error);
  CHECK(
      p->service("ovr_IAP_ConsumePurchase", {{std::string{}}}, {}).error.code ==
      ErrorCode::invalid_argument);
  optest::Arena arena;
  Session s(optest::app(), p, arena.memory());
  REQUIRE(s.initialize(optest::app().app_id) == InitResult::success);
  optest::invoke(s, "ovr_IAP_GetProductsBySKU", {0, 1});
  auto m = optest::wait_pop(s);
  CHECK(s.get(m, "ovr_Message_IsError") == 1);
  s.free_message(m);
  const char checkout_sku[] = "fixture-sku";
  auto checkout_request = optest::invoke(s, "ovr_IAP_LaunchCheckoutFlow", {venture::h2g(checkout_sku)});
  auto checkout = optest::wait_pop(s);
  CHECK(s.get(checkout, "ovr_Message_GetRequestID") == checkout_request);
  CHECK(s.get(checkout, "ovr_Message_GetType") == 0x3f9b0d0d);
  CHECK(s.get(checkout, "ovr_Message_IsError") == 1);
  CHECK(optest::invoke(s, "ovr_Message_GetNativeMessage", {checkout}) == checkout);
  CHECK(s.get(checkout, "ovr_Message_GetPurchase") == 0);
  s.free_message(checkout);
  CHECK(server.requests().size() == 6);
}

TEST_CASE(
    "Platform durable cache commits complete traversals, excludes consumables "
    "and isolates accounts") {
  const auto root =
      std::filesystem::temp_directory_path() /
      ("venture-iap-cache-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  struct Cleanup {
    std::filesystem::path root;
    ~Cleanup() {
      std::filesystem::remove(root / "iap-durables.json");
      std::filesystem::remove(root);
    }
  } cleanup{root};
  IapCache cache(root);
  CHECK(cache.read("42").error.code == ErrorCode::unavailable);
  auto original = purchase_fixture("original", "DURABLE");
  original["access_token"] = "fixture-secret";
  cache.page("42", {}, Json{{"data", Json::array({original})}},
             cache.generation());
  REQUIRE_FALSE(cache.read("42").error);
  CHECK(venture::paths::read(root / "iap-durables.json").find("fixture-secret") ==
        std::string::npos);
  const auto next =
      "https://graph.oculus.com/v1.20/application/"
      "viewer_purchases?after=fixture";
  cache.page(
      "42", {},
      Json{{"data", Json::array({purchase_fixture("replacement", "DURABLE")})},
           {"paging", {{"next", next}}}},
      cache.generation());
  CHECK(std::get<std::string>(cache.read("42").model.elements.at(0).fields.at(
            "SKU")) == "original");
  cache.page(
      "42", next,
      Json{{"data",
            Json::array({purchase_fixture("consumable", "CONSUMABLE"),
                         purchase_fixture("subscription", "SUBSCRIPTION")})}},
      cache.generation());
  auto r = cache.read("42");
  REQUIRE_FALSE(r.error);
  REQUIRE(r.model.elements.size() == 1);
  CHECK(std::get<std::string>(r.model.elements[0].fields.at("SKU")) ==
        "replacement");
  CHECK(cache.read("84").error.code == ErrorCode::invalid_response);
  IapCache reopened(root);
  CHECK(reopened.read("42").model.elements.size() == 1);
  const auto before_consume = cache.generation();
  cache.consumed("42", "replacement");
  CHECK(cache.read("42").model.elements.empty());
  cache.page(
      "42", {},
      Json{{"data", Json::array({purchase_fixture("stale", "DURABLE")})}},
      before_consume);
  CHECK(cache.read("42").model.elements.empty());
  cache.page("42", {}, Json{{"data", Json::array()}}, cache.generation());
  CHECK_FALSE(cache.read("42").error);
  const auto bytes = venture::paths::read(root / "iap-durables.json");
  CHECK(bytes.find("after=") == bytes.npos);
  CHECK(bytes.find("access_token") == bytes.npos);
}

TEST_CASE(
    "Platform durable cache is available without reauth and is revoked when "
    "credentials change") {
  const auto root =
      std::filesystem::temp_directory_path() /
      ("venture-iap-offline-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  struct Cleanup {
    std::filesystem::path root;
    ~Cleanup() {
      std::filesystem::remove(root / "iap-durables.json");
      std::filesystem::remove(root);
    }
  } cleanup{root};
  class Store final : public SecretStore {
   public:
    optest::FakeStore credential;
    std::filesystem::path root;
    Credential load() override { return credential.load(); }
    std::filesystem::path platform_directory(std::string_view) const override {
      return root;
    }
  };
  auto store = std::make_shared<Store>();
  store->root = root;
  Server server;
  auto online = service_provider(server, store);
  server.push(Json{
      {"data", Json::array({purchase_fixture("owned-durable", "DURABLE")})}}
                  .dump());
  CHECK_FALSE(online->service("ovr_IAP_GetViewerPurchases", {}, {}).error);
  auto offline = owned_library_provider(optest::app(), store,
                                        loopback_http_transport(server.port));
  REQUIRE_FALSE(offline->prepare());
  auto r = offline->service("ovr_IAP_GetViewerPurchasesDurableCache", {}, {});
  REQUIRE_FALSE(r.error);
  REQUIRE(r.model.elements.size() == 1);
  CHECK(std::get<std::string>(r.model.elements[0].fields.at("SKU")) ==
        "owned-durable");
  CHECK(server.requests().size() == 3);
  store->credential.replace();
  CHECK(offline->service("ovr_IAP_GetViewerPurchasesDurableCache", {}, {})
            .error.code == ErrorCode::identity_mismatch);
  CHECK(server.requests().size() == 3);
}
