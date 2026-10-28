#include "hle/ovrplatform/social.h"

#include <charconv>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace venture::ovrplatform {
namespace {
using Json = nlohmann::json;
// Signed Meta Platform 1.205: USER literal RVA 0x896880, current_room
// appended by initializer 0x70980; DESTINATION literal RVA 0x896d30.
constexpr auto user_fields =
    "id,alias,display_name,presence,presence_deeplink_message,"
    "presence_destination_api_name,presence_lobby_session_id,"
    "presence_match_session_id,presence_status,profile_url,profile_url_small,current_room";
constexpr auto destination_fields = "api_name,display_name,deeplink_message,shareable_uri";
std::string text(const Json& j, const char* key) {
  const auto it = j.find(key);
  if (it == j.end() || it->is_null()) return {};
  if (!it->is_string() || it->get_ref<const std::string&>().find('\0') != std::string::npos)
    throw std::invalid_argument("Invalid social string");
  return it->get<std::string>();
}
std::uint64_t id(const Json& j) {
  const auto& v = j.at("id");
  auto s = v.is_string()            ? v.get<std::string>()
           : v.is_number_unsigned() ? std::to_string(v.get<std::uint64_t>())
                                    : std::string{};
  if (!valid_id(s)) throw std::invalid_argument("Invalid social ID");
  std::uint64_t result = 0;
  std::from_chars(s.data(), s.data() + s.size(), result);
  return result;
}
Model user(const Json& j) {
  auto m = user_model(id(j), text(j, "alias"), text(j, "display_name"));
  for (auto [field, key] : {std::pair{"ImageUrl", "profile_url"},
                            {"SmallImageUrl", "profile_url_small"},
                            {"Presence", "presence"},
                            {"PresenceDeeplinkMessage", "presence_deeplink_message"},
                            {"PresenceDestinationApiName", "presence_destination_api_name"},
                            {"PresenceLobbySessionId", "presence_lobby_session_id"},
                            {"PresenceMatchSessionId", "presence_match_session_id"}})
    m.fields.emplace(field, text(j, key));
  const auto status = text(j, "presence_status");
  // OVR_UserPresenceStatus.h; signed 1.205 FromString RVA 0x1973b0
  // confirms uppercase ONLINE=1 / OFFLINE=2, everything else Unknown=0.
  m.fields.emplace("PresenceStatus", std::uint32_t(status == "ONLINE"    ? 1
                                                   : status == "OFFLINE" ? 2
                                                                         : 0));
  return m;
}
Model destination(const Json& j) {
  if (!j.is_object() || text(j, "api_name").empty())
    throw std::invalid_argument("Invalid destination");
  return {"Destination",
          {{"ApiName", text(j, "api_name")},
           {"DisplayName", text(j, "display_name")},
           {"DeeplinkMessage", text(j, "deeplink_message")},
           {"ShareableUri", text(j, "shareable_uri")}}};
}
Model invite(const Json& j) {
  Model m{"ApplicationInvite",
          {{"ID", id(j)},
           {"LobbySessionId", text(j, "lobby_session_id")},
           {"MatchSessionId", text(j, "match_session_id")}}};
  if (j.contains("is_active") && !j["is_active"].is_null()) {
    if (!j["is_active"].is_boolean()) throw std::invalid_argument("Invalid invite state");
    m.fields.emplace("IsActive", j["is_active"].get<bool>());
  }
  for (auto [key, field] : {std::pair{"destination", "Destination"}, {"recipient", "Recipient"}})
    if (j.contains(key) && !j[key].is_null())
      m.fields.emplace(field, std::make_shared<const Model>(key == std::string_view("destination")
                                                                ? destination(j[key])
                                                                : user(j[key])));
  return m;
}
Model array(const Json& j, std::string_view kind) {
  const auto& data = j.at("data");
  if (!data.is_array()) throw std::invalid_argument("Invalid social array");
  Model m{std::string(kind)};
  for (const auto& entry : data)
    m.elements.push_back(kind == "UserArray"          ? user(entry)
                         : kind == "DestinationArray" ? destination(entry)
                                                      : invite(entry));
  if (j.contains("paging") && !j["paging"].is_null()) {
    if (!j["paging"].is_object()) throw std::invalid_argument("Invalid social paging");
    for (auto [key, field] : {std::pair{"next", "NextUrl"}, {"previous", "PreviousUrl"}}) {
      auto url = text(j["paging"], key);
      if (!url.empty() && !valid_next_page_url(url))
        throw std::invalid_argument("Untrusted social page");
      m.fields.emplace(field, std::move(url));
    }
  }
  return m;
}
std::string join_ids(const std::vector<std::string>& ids) {
  if (ids.size() > 4096) throw std::invalid_argument("Too many social IDs");
  std::string result;
  for (const auto& id : ids) {
    if (!valid_id(id)) throw std::invalid_argument("Invalid social ID argument");
    if (!result.empty()) result += ',';
    result += id;
  }
  return result;
}
struct Route {
  const char *name, *endpoint, *kind;
  bool page = false;
};
constexpr Route routes[] = {
    {"ovr_User_GetLoggedInUserFriends", "social-friends", "UserArray"},
    {"ovr_User_GetNextUserArrayPage", "social-friends", "UserArray", true},
    {"ovr_RichPresence_GetDestinations", "social-destinations", "DestinationArray"},
    {"ovr_RichPresence_GetNextDestinationArrayPage", "social-destinations", "DestinationArray",
     true},
    {"ovr_GroupPresence_GetInvitableUsers", "social-invitable", "UserArray"},
    {"ovr_GroupPresence_SendInvites", "social-send-invites", "SendInvitesResult"},
    // HTTP v2's absolute paging builder uses any registered GET route after
    // validating the server URL. No initial sent-invites endpoint is enabled.
    {"ovr_GroupPresence_GetNextApplicationInviteArrayPage", "social-friends",
     "ApplicationInviteArray", true},
};
}  // namespace
void register_social_endpoints(HttpTransport& http) {
  // Builders 0xf8f40, 0x163ee0, 0x1630b0 and 0x1636d0 respectively.
  http.register_endpoint({"social-friends", HttpMethod::get, "/me/friends", {"fields"}});
  http.register_endpoint(
      {"social-destinations", HttpMethod::get, "/application/destinations", {"fields"}});
  http.register_endpoint({"social-invitable",
                          HttpMethod::get,
                          "/me/application_invitable_users",
                          {"fields", "suggested_user_ids"}});
  http.register_endpoint({"social-send-invites",
                          HttpMethod::post,
                          "/me/send_application_invites",
                          {"fields", "recipient_ids"}});
}
Result social_request(Provider& provider, std::string_view name, const RequestArgs& args,
                      std::stop_token stop, bool retry_reads) {
  if (args.error) return {args.error};
  const Route* route = nullptr;
  for (const auto& r : routes)
    if (name == r.name) route = &r;
  if (!route) return {failure(ErrorCode::unsupported)};
  HttpRequest request{route->endpoint};
  request.retry_reads = retry_reads;
  try {
    if (route->page) {
      if (args.values.size() != 1) throw std::invalid_argument("Invalid page arguments");
      const auto& page = std::get<PageSnapshot>(args.values[0]);
      if (page.kind != route->kind || page.next_url.empty() || !valid_next_page_url(page.next_url))
        throw std::invalid_argument("Invalid page");
      request.next_url = page.next_url;
    } else if (name == "ovr_GroupPresence_SendInvites") {
      if (args.values.size() != 2) throw std::invalid_argument("Invalid invite arguments");
      const auto& ids = std::get<std::vector<Id>>(args.values[0]);
      if (ids.empty() || ids.size() != std::get<std::uint32_t>(args.values[1]))
        throw std::invalid_argument("Invalid recipients");
      std::vector<std::string> strings;
      for (auto id : ids) strings.push_back(std::to_string(id));
      request.parameters = {
          {"recipient_ids", join_ids(strings)},
          {"fields", "invites{id,destination{" + std::string(destination_fields) + "},recipient{" +
                         user_fields + "},is_active,lobby_session_id,match_session_id}"}};
    } else {
      request.parameters.emplace_back("fields", route->kind == std::string_view("DestinationArray")
                                                    ? destination_fields
                                                    : user_fields);
      if (name == "ovr_GroupPresence_GetInvitableUsers") {
        if (args.values.size() != 1) throw std::invalid_argument("Invalid invite options");
        const auto& options = std::get<OptionsSnapshot>(args.values[0]);
        if (options.kind != "InviteOptions") throw std::invalid_argument("Invalid options kind");
        for (const auto& [key, values] : options.fields) {
          if (key != "SuggestedUsers") throw std::invalid_argument("Unknown invite option");
          if (!values.empty())
            request.parameters.emplace_back("suggested_user_ids", join_ids(values));
        }
      } else if (!args.values.empty())
        throw std::invalid_argument("Unexpected social arguments");
    }
  } catch (...) {
    return {failure(ErrorCode::invalid_argument)};
  }
  const auto response = provider.graph_request(std::move(request), stop);
  if (auto error = http_response_error(response)) return {std::move(error)};
  try {
    auto body = Json::parse(response.body);
    auto model = route->kind == std::string_view("SendInvitesResult")
                     ? Model{"SendInvitesResult",
                             {{"Invites", std::make_shared<const Model>(array(
                                              body.at("invites"), "ApplicationInviteArray"))}}}
                     : array(body, route->kind);
    return {{ErrorCode::none, response.status, {}}, {}, {}, std::move(model)};
  } catch (...) {
    auto error = failure(ErrorCode::invalid_response);
    error.http_status = response.status;
    return {std::move(error)};
  }
}
void register_social_handlers(Session& session) {
  for (const auto& route : routes)
    session.register_handler(route.name, {[name = route.name](Provider& p, const RequestArgs& args,
                                                              std::stop_token stop) {
                                            return social_request(p, name, args, stop);
                                          },
                                          route.kind});
  // Meta's standalone push parser (0xdcc30) has no GroupPresence join/leave
  // intent or InvitationsSent source. Do not infer notifications from REST.
}
}  // namespace venture::ovrplatform
