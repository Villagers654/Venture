// Runtime-side diagnostics: the account session is decrypted only here, never
// by a build tool or a test runner. Output contains no tokens, nonces or user IDs.
#include <charconv>
#include <chrono>
#include <iostream>
#include <thread>

#include "common/meta/accounts.h"
#include "core/guest.h"
#include "hle/ovrplatform/progression.h"
#include "hle/ovrplatform/provider.h"
#include "hle/ovrplatform/identity_iap.h"
#include <nlohmann/json.hpp>
#include "hle/ovrplatform/social.h"

int main(int argc, char** argv) {
  using namespace venture::ovrplatform;
  std::string_view account_key;
  if (argc >= 6 && std::string_view(argv[argc - 2]) == "--account") {
    account_key = argv[argc - 1];
    if (!venture::meta::valid_account_key(account_key)) {
      std::cerr << "Invalid account slot key\n";
      return 2;
    }
    argc -= 2;
  }
  if (argc < 4 || argc > 6) {
    std::cerr << "usage: venture-runtime-platform-check PLAN APK_PACKAGE "
                 "APK_VERSION [--probe-auth | --probe-age | --probe-identity | --probe-iap | "
                 "--probe-social | --heartbeat | --probe-leaderboard NAME | "
                 "--probe-leaderboard-after-rank NAME | "
                 "--probe-achievement-definitions | --probe-leaderboard-metadata NAME | "
                 "--probe-achievement-progress] [--account <12-hex-key>]\n";
    return 2;
  }
  try {
    std::int64_t version = 0;
    const std::string text = argv[3];
    auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), version);
    if (error != std::errc{} || end != text.data() + text.size()) return 2;
    auto app = read_app_identity(argv[1], argv[2], version);
    auto store = account_key.empty() ? runtime_secret_store()
                                     : runtime_secret_store(account_key);
    auto http = meta_http_transport();
    auto provider = owned_library_provider(app, store, http);
    auto prepared = provider->prepare();
    std::cout << "initialize_error=" << static_cast<int>(prepared.code) << '\n';
    if (prepared) return 1;
    if (argc == 5 && std::string_view(argv[4]) == "--heartbeat") {
      const auto result = provider->heartbeat("", {});
      std::cout << "heartbeat_error=" << static_cast<int>(result.error.code);
      if (result.error) std::cout << " http=" << result.error.http_status;
      else std::cout << " interval_seconds=" << result.text;
      std::cout << '\n';
      return result.error ? 1 : 0;
    }
    if (argc == 5 && (std::string_view(argv[4]) == "--probe-age" ||
                      std::string_view(argv[4]) == "--probe-identity" ||
                      std::string_view(argv[4]) == "--probe-iap")) {
      // Explicit bounded reads only. No game writes or cache writes are probed.
      const auto auth = provider->access_token({});
      if (auth.error) return 1;
      const bool identity = std::string_view(argv[4]) == "--probe-identity";
      const std::vector<std::string_view> names =
          std::string_view(argv[4]) == "--probe-age"
              ? std::vector<std::string_view>{"ovr_UserAgeCategory_Get"}
          : identity
              ? std::vector<std::string_view>{"ovr_User_Get",
                                              "ovr_User_GetOrgScopedID"}
              : std::vector<std::string_view>{"ovr_IAP_GetProductsBySKU",
                                              "ovr_IAP_GetViewerPurchases"};
      bool failed = false;
      for (auto name : names) {
        HttpRequest request;
        if (name == "ovr_IAP_GetProductsBySKU")
          request = identity_iap_request(
              name, {{std::vector<std::string>{}, std::int32_t{0}}});
        else
          request = identity_iap_request(name, {{auth.user.id}});
        request.bearer = auth.text;
        request.retry_reads = false;
        const auto response = http->send(request, {});
        auto error = http_response_error(response);
        Model model;
        if (!error) {
          try {
            model =
                identity_iap_model(name, nlohmann::json::parse(response.body));
            if (name == "ovr_User_Get" &&
                std::get<std::uint64_t>(model.fields.at("ID")) != auth.user.id)
              error = failure(ErrorCode::invalid_response);
          } catch (...) {
            error = failure(ErrorCode::invalid_response);
          }
        }
        std::cout << "service=" << name << " http=" << response.status
                  << " error=" << static_cast<int>(error.code)
                  << " elements=" << model.elements.size();
        if (model.kind == "UserAccountAgeCategory")
          std::cout << " age_enum="
                    << std::get<std::int32_t>(model.fields.at("AgeCategory"));
        std::cout << '\n';
        failed |= bool(error);
      }
      return failed ? 1 : 0;
    }
    if (argc == 5 && std::string_view(argv[4]) == "--probe-social") {
      bool failed = false;
      for (auto name : {"ovr_User_GetLoggedInUserFriends", "ovr_RichPresence_GetDestinations",
                        "ovr_GroupPresence_GetInvitableUsers"}) {
        RequestArgs args;
        if (std::string_view(name) == "ovr_GroupPresence_GetInvitableUsers")
          args.values.emplace_back(OptionsSnapshot{"InviteOptions", {}});
        const auto result = social_request(*provider, name, args, {}, false);
        std::cout << name << " error=" << static_cast<int>(result.error.code)
                  << " http=" << result.error.http_status << " model=" << result.model.kind
                  << " count=" << result.model.elements.size() << '\n';
        failed |= bool(result.error);
      }
      return failed ? 1 : 0;
    }
    Result entitlement;
    // Progression probes perform only their requested read plus the existing
    // app-token exchange/identity check, without another library query.
    if (argc == 4 || (argc == 5 && std::string_view(argv[4]) == "--probe-auth")) {
      entitlement = provider->entitlement({});
      std::cout << "entitlement_error=" << static_cast<int>(entitlement.error.code)
                << " http=" << entitlement.error.http_status << '\n';
    }
    if (argc == 5 && std::string_view(argv[4]) == "--probe-auth") {
      auto result = probe_application_auth(app, *store, *http);
      std::cout << "exchange_http=" << result.exchange_status
                << " token_returned=" << result.token_returned
                << " token_differs_from_profile="
                << result.token_differs_from_profile
                << " identity_http=" << result.identity_status
                << " identity_returned=" << result.identity_returned
                << " identity_matches_profile="
                << result.identity_matches_profile
                << " scope_http=" << result.scope_status
                << " scope_confirmed=" << result.app_scope_confirmed
                << " probe_error=" << static_cast<int>(result.error.code)
                << '\n';
      const auto user = provider->logged_in_user({});
      std::cout << "provider_identity_error=" << static_cast<int>(user.error.code)
                << " http=" << user.error.http_status
                << " identity_returned=" << (user.user.id != 0) << '\n';
    } else if (argc >= 5) {
      const std::string_view option = argv[4];
      std::string_view call, kind;
      RequestArgs args;
      if (option == "--probe-leaderboard" && argc == 6) {
        call = "ovr_Leaderboard_GetEntries";
        kind = "LeaderboardEntryArray";
        args.values = {std::string(argv[5]), std::int32_t{1}, EnumValue{0}, EnumValue{0}};
      } else if (option == "--probe-leaderboard-after-rank" && argc == 6) {
        call = "ovr_Leaderboard_GetEntriesAfterRank";
        kind = "LeaderboardEntryArray";
        args.values = {std::string(argv[5]), std::int32_t{100},
                       std::uint64_t{0}};
      } else if (option == "--probe-achievement-definitions" && argc == 5) {
        call = "ovr_Achievements_GetAllDefinitions";
        kind = "AchievementDefinitionArray";
      } else if (option == "--probe-leaderboard-metadata" && argc == 6) {
        call = "ovr_Leaderboard_Get";
        kind = "LeaderboardArray";
        args.values = {std::string(argv[5])};
      } else if (option == "--probe-achievement-progress" && argc == 5) {
        call = "ovr_Achievements_GetAllProgress";
        kind = "AchievementProgressArray";
      } else
        return 2;
      Memory memory{[](std::size_t n) { return venture::h2g(::operator new(n)); },
                    [](Handle h) { ::operator delete(venture::g2h(h)); }};
      Session session(app, provider, memory);
      register_progression_handlers(session, {true, true, true, true, false});
      if (session.initialize(app.app_id) != InitResult::success)
        return 1;
      for (const auto& spec : request_specs())
        if (call == spec.name)
          session.request(spec, std::move(args));
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
      Handle message = 0;
      while (!(message = session.pop()) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      if (!message)
        return 1;
      auto error = session.get(message, "ovr_Message_GetError");
      auto model = session.get(message, "ovr_Message_Get" + std::string(kind));
      std::cout << "progression_probe=" << call
                << " error=" << static_cast<std::int32_t>(session.get(error, "ovr_Error_GetCode"))
                << " error_http=" << session.get(error, "ovr_Error_GetHttpCode")
                << " elements=" << session.get(model, "ovr_" + std::string(kind) + "_GetSize")
                << '\n';
      session.free_message(message);
      if (error)
        return 1;
    }
    return entitlement.error ? 1 : 0;
  } catch (...) {
    std::cerr << "Platform diagnostic could not read the launch identity or "
                 "session\n";
    return 1;
  }
}
