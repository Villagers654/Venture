#include "hle/ovrplatform/progression.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <type_traits>

#include "common/log.h"

namespace venture::ovrplatform {
namespace {
using Json = nlohmann::json;
constexpr std::string_view entry_fields =
    "id,user{id,alias,display_name,presence,presence_deeplink_message,"
    "presence_destination_api_name,presence_lobby_session_id,presence_match_"
    "session_id,"
    "presence_status,profile_url,profile_url_small},rank,score,display_score,"
    "timestamp,"
    "extra_data_base64,supplementary_metric{metric}";
constexpr std::string_view definition_fields = "api_name,achievement_type,target,bitfield_length";
constexpr std::string_view progress_fields =
    "definition{api_name},bitfield_progress,count_progress,is_unlocked,unlock_"
    "time";
struct Operation {
  const char *name, *endpoint, *kind;
};
constexpr Operation operations[] = {
    {"ovr_Leaderboard_Get", "leaderboards", "LeaderboardArray"},
    {"ovr_Leaderboard_GetEntries", "leaderboard-entries", "LeaderboardEntryArray"},
    {"ovr_Leaderboard_GetEntriesAfterRank", "leaderboard-entries", "LeaderboardEntryArray"},
    {"ovr_Leaderboard_GetEntriesByIds", "leaderboard-entries", "LeaderboardEntryArray"},
    {"ovr_Leaderboard_GetNextEntries", "leaderboard-entries", "LeaderboardEntryArray"},
    {"ovr_Leaderboard_GetPreviousEntries", "leaderboard-entries", "LeaderboardEntryArray"},
    {"ovr_Leaderboard_GetNextLeaderboardArrayPage", "leaderboards", "LeaderboardArray"},
    {"ovr_Leaderboard_WriteEntry", "leaderboard-write", "LeaderboardUpdateStatus"},
    {"ovr_Leaderboard_WriteEntryWithSupplementaryMetric", "leaderboard-write",
     "LeaderboardUpdateStatus"},
    {"ovr_Achievements_GetAllDefinitions", "achievement-definitions", "AchievementDefinitionArray"},
    {"ovr_Achievements_GetDefinitionsByName", "achievement-definitions",
     "AchievementDefinitionArray"},
    {"ovr_Achievements_GetNextAchievementDefinitionArrayPage", "achievement-definitions",
     "AchievementDefinitionArray"},
    {"ovr_Achievements_GetAllProgress", "achievement-progress", "AchievementProgressArray"},
    {"ovr_Achievements_GetProgressByName", "achievement-progress", "AchievementProgressArray"},
    {"ovr_Achievements_GetNextAchievementProgressArrayPage", "achievement-progress",
     "AchievementProgressArray"},
    {"ovr_Achievements_Unlock", "achievement-write", "AchievementUpdate"},
    {"ovr_Achievements_AddCount", "achievement-write", "AchievementUpdate"},
    {"ovr_Achievements_AddFields", "achievement-write", "AchievementUpdate"},
};
void require(bool condition) {
  if (!condition)
    throw std::invalid_argument("Invalid Platform progression value");
}
std::string text(const Json& j, const char* key, bool optional = false) {
  auto it = j.find(key);
  if (optional && (it == j.end() || it->is_null()))
    return {};
  require(it != j.end() && it->is_string());
  auto value = it->get<std::string>();
  require(value.find('\0') == value.npos);
  return value;
}
template <class T> T number(const Json& j) {
  // Graph encodes scores/counts/IDs as decimal strings; timestamp/rank are
  // JSON integers (Meta Platform 1.205 src/models/*::deserialize).
  if (j.is_string()) {
    const auto& value = j.get_ref<const std::string&>();
    T out{};
    auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), out);
    require(error == std::errc{} && end == value.data() + value.size());
    return out;
  }
  require(j.is_number_integer());
  if (j.is_number_unsigned()) {
    auto value = j.get<std::uint64_t>();
    require(value <= static_cast<std::uint64_t>(std::numeric_limits<T>::max()));
    return static_cast<T>(value);
  }
  auto value = j.get<std::int64_t>();
  if constexpr (std::is_unsigned_v<T>)
    require(value >= 0);
  else
    require(value >= std::numeric_limits<T>::min());
  require(static_cast<std::uint64_t>(value >= 0 ? value : 0) <=
          static_cast<std::uint64_t>(std::numeric_limits<T>::max()));
  return static_cast<T>(value);
}
Id id(const Json& j) {
  auto value = number<Id>(j);
  require(value != 0);
  return value;
}
bool boolean(const Json& j, const char* key) {
  require(j.at(key).is_boolean());
  return j.at(key).get<bool>();
}
constexpr std::string_view alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string encode(const Blob& bytes) {
  std::string out;
  for (std::size_t i = 0; i < bytes.size(); i += 3) {
    auto n = std::uint32_t(std::to_integer<unsigned>(bytes[i])) << 16;
    if (i + 1 < bytes.size())
      n |= std::to_integer<unsigned>(bytes[i + 1]) << 8;
    if (i + 2 < bytes.size())
      n |= std::to_integer<unsigned>(bytes[i + 2]);
    out += alphabet[(n >> 18) & 63];
    out += alphabet[(n >> 12) & 63];
    out += i + 1 < bytes.size() ? alphabet[(n >> 6) & 63] : '=';
    out += i + 2 < bytes.size() ? alphabet[n & 63] : '=';
  }
  return out;
}
Blob decode(std::string_view value) {
  require(value.size() % 4 == 0 && value.size() <= 2732); // OVR_Requests_Leaderboard.h: 2 KiB
  Blob out;
  for (std::size_t i = 0; i < value.size(); i += 4) {
    unsigned n = 0, padding = 0;
    for (unsigned j = 0; j < 4; ++j) {
      const auto c = value[i + j];
      if (c == '=') {
        require(i + 4 == value.size() && j >= 2);
        ++padding;
        n <<= 6;
      } else {
        auto at = alphabet.find(c);
        require(!padding && at != alphabet.npos);
        n = (n << 6) | static_cast<unsigned>(at);
      }
    }
    require(padding <= 2 && (!padding || (n & (padding == 2 ? 0xffffu : 0xffu)) == 0));
    out.push_back(std::byte((n >> 16) & 255));
    if (padding < 2)
      out.push_back(std::byte((n >> 8) & 255));
    if (!padding)
      out.push_back(std::byte(n & 255));
  }
  require(out.size() <= 2048);
  return out;
}
Model user(const Json& j, std::string_view& stage) {
  stage = "entry.user.id";
  auto user_id = number<Id>(j.at("id"));
  // Signed Meta Platform 1.205 ovrID_FromString (RVA 0xe9cf0,
  // decimal parser 0x13f760) accepts zero. Preserve the server's public user
  // ID even when it is unavailable; app authentication still requires an ID.
  stage = "entry.user.alias";
  auto alias = text(j, "alias", true);
  stage = "entry.user.display_name";
  Model m = user_model(user_id, std::move(alias), text(j, "display_name", true));
  for (auto [key, field] : {std::pair{"presence", "Presence"},
                            {"presence_deeplink_message", "PresenceDeeplinkMessage"},
                            {"presence_destination_api_name", "PresenceDestinationApiName"},
                            {"presence_lobby_session_id", "PresenceLobbySessionId"},
                            {"presence_match_session_id", "PresenceMatchSessionId"},
                            {"profile_url", "ImageUrl"},
                            {"profile_url_small", "SmallImageUrl"}}) {
    stage = field;
    m.fields[field] = text(j, key, true);
  }
  stage = "entry.user.presence_status";
  auto presence = text(j, "presence_status", true);
  m.fields["PresenceStatus"] = std::int32_t{presence == "ONLINE"    ? 1
                                            : presence == "OFFLINE" ? 2
                                                                    : 0};
  return m;
}
Model element(const Json& j, std::string_view kind, std::string_view& stage) {
  require(j.is_object());
  if (kind == "LeaderboardArray") {
    Model m{"Leaderboard", {{"ApiName", text(j, "api_name")}, {"ID", id(j.at("id"))}}};
    if (j.contains("destination") && !j["destination"].is_null()) {
      const auto& d = j["destination"];
      m.fields["Destination"] = std::make_shared<const Model>(
          Model{"Destination",
                {{"ApiName", text(d, "api_name")},
                 {"DisplayName", text(d, "display_name")},
                 {"DeeplinkMessage", text(d, "deeplink_message", true)},
                 {"ShareableUri", text(d, "shareable_uri", true)}}});
    }
    return m;
  }
  if (kind == "LeaderboardEntryArray") {
    stage = "entry.extra_data_base64";
    auto bytes = decode(text(j, "extra_data_base64", true));
    stage = "entry.rank";
    auto rank = number<std::int32_t>(j.at("rank"));
    stage = "entry.score";
    auto score = number<std::int64_t>(j.at("score"));
    stage = "entry.timestamp";
    auto timestamp = number<std::uint64_t>(j.at("timestamp"));
    stage = "entry.display_score";
    Model m{"LeaderboardEntry",
            {{"Rank", rank},
             {"Score", score},
             {"Timestamp", timestamp},
             {"DisplayScore", text(j, "display_score", true)},
             {"ExtraDataLength", static_cast<std::uint32_t>(bytes.size())},
             {"ExtraData", std::move(bytes)}}};
    stage = "entry.id";
    if (j.contains("id") && !j["id"].is_null())
      m.fields["ID"] = id(j["id"]);
    stage = "entry.user";
    if (j.contains("user") && !j["user"].is_null())
      m.fields["User"] = std::make_shared<const Model>(user(j["user"], stage));
    if (j.contains("supplementary_metric") && !j["supplementary_metric"].is_null()) {
      const auto& metric = j["supplementary_metric"];
      stage = "entry.supplementary_metric.id";
      auto metric_id = id(metric.at("id"));
      stage = "entry.supplementary_metric.metric";
      m.fields["SupplementaryMetric"] = std::make_shared<const Model>(
          Model{"SupplementaryMetric",
                {{"ID", metric_id}, {"Metric", number<std::int64_t>(metric.at("metric"))}}});
    }
    return m;
  }
  if (kind == "AchievementDefinitionArray") {
    auto type = text(j, "achievement_type");
    std::int32_t value = type == "SIMPLE" ? 1 : type == "BITFIELD" ? 2 : type == "COUNT" ? 3 : 0;
    return {"AchievementDefinition",
            {{"Name", text(j, "api_name")},
             {"Type", value},
             {"Target", number<std::uint64_t>(j.at("target"))},
             {"BitfieldLength", number<std::uint32_t>(j.at("bitfield_length"))}}};
  }
  return {"AchievementProgress",
          {{"Name", text(j.at("definition"), "api_name")},
           {"Bitfield", text(j, "bitfield_progress", true)},
           {"Count", number<std::uint64_t>(j.at("count_progress"))},
           {"IsUnlocked", boolean(j, "is_unlocked")},
           {"UnlockTime", number<std::uint64_t>(j.at("unlock_time"))}}};
}
Model parse(const Json& j, std::string_view kind, std::string_view& stage) {
  stage = "root";
  require(j.is_object());
  if (kind == "AchievementUpdate")
    return {std::string(kind),
            {{"Name", text(j, "api_name")}, {"JustUnlocked", boolean(j, "just_unlocked")}}};
  if (kind == "LeaderboardUpdateStatus") {
    std::vector<Id> ids;
    if (j.contains("updated_challenge_ids")) {
      require(j["updated_challenge_ids"].is_array());
      for (const auto& v : j["updated_challenge_ids"])
        ids.push_back(id(v));
    }
    return {std::string(kind),
            {{"DidUpdate", boolean(j, "did_update")}, {"UpdatedChallengeId", std::move(ids)}}};
  }
  require(j.at("data").is_array());
  Model m{std::string(kind)};
  stage = "elements";
  for (const auto& v : j["data"])
    m.elements.push_back(element(v, kind, stage));
  stage = "paging";
  if (j.contains("paging") && !j["paging"].is_null()) {
    require(j["paging"].is_object());
    for (auto [key, field] : {std::pair{"next", "NextUrl"}, {"previous", "PreviousUrl"}}) {
      stage = key == std::string_view("next") ? "paging.next" : "paging.previous";
      auto url = text(j["paging"], key, true);
      require(url.empty() || valid_next_page_url(url));
      m.fields[field] = std::move(url);
    }
  }
  stage = "summary.total_count";
  if (kind == "LeaderboardEntryArray")
    m.fields["TotalCount"] = number<std::uint64_t>(j.at("summary").at("total_count"));
  return m;
}
template <class T> const T& arg(const RequestArgs& args, std::size_t i) {
  return std::get<T>(args.values.at(i));
}
std::string name(const RequestArgs& args) {
  auto value = arg<std::string>(args, 0);
  require(!value.empty());
  return value;
}
void start_at(Fields& fields, std::int32_t start) {
  require(start >= 0 && start <= 2);
  if (start)
    fields.emplace_back("start_at",
                        start == 1 ? "CENTERED_ON_VIEWER" : "CENTERED_ON_VIEWER_OR_TOP");
}
Result execute(Provider& provider, const Operation& op, const RequestArgs& args,
               ProgressionPolicy policy, std::stop_token stop) {
  if (args.error)
    return {args.error};
  const std::string_view call = op.name, endpoint = op.endpoint, kind = op.kind;
  if ((endpoint == "leaderboards" && !policy.leaderboard_metadata_reads) ||
      (endpoint == "achievement-progress" && !policy.achievement_progress_reads) ||
      (endpoint == "leaderboard-write" && !policy.leaderboard_writes) ||
      (endpoint == "achievement-write" && !policy.achievement_writes))
    return {failure(ErrorCode::unsupported)};
  Fields fields;
  std::string page;
  try {
    // Shapes from signed Meta Platform 1.205 request builders, plan sections
    // 2.6/2.7. All POST values belong in the query; the transport sends no
    // body.
    if (call.find("GetNext") != call.npos || call.find("GetPrevious") != call.npos) {
      const auto& snapshot = arg<PageSnapshot>(args, 0);
      require(snapshot.kind == kind);
      page = call.find("GetPrevious") != call.npos ? snapshot.previous_url : snapshot.next_url;
      require(!page.empty() && valid_next_page_url(page));
    } else if (endpoint == "leaderboards") {
      fields = {{"api_name", name(args)},
                {"fields", "api_name,id,destination{api_name,display_name,"
                           "deeplink_message,shareable_uri}"}};
    } else if (endpoint == "leaderboard-entries") {
      auto limit = arg<std::int32_t>(args, 1);
      require(limit > 0);
      fields = {{"api_name", name(args)},
                {"fields", std::string(entry_fields)},
                {"limit", std::to_string(limit)},
                {"summary", "true"}};
      if (call.ends_with("AfterRank")) {
        fields.emplace_back("start_at", "OFFSET");
        fields.emplace_back("offset", std::to_string(arg<std::uint64_t>(args, 2)));
      } else if (call.ends_with("ByIds")) {
        const auto& ids = arg<std::vector<Id>>(args, 3);
        require(ids.size() == arg<std::uint32_t>(args, 4) &&
                std::all_of(ids.begin(), ids.end(), [](Id v) { return v != 0; }));
        fields.emplace_back("filter", "USER_IDS");
        fields.emplace_back("user_ids", Json(ids).dump());
        start_at(fields, arg<EnumValue>(args, 2).value);
      } else {
        const auto filter = arg<EnumValue>(args, 2).value;
        require(filter == 0 || filter == 1);
        if (filter == 1)
          fields.emplace_back("filter", "FRIENDS");
        start_at(fields, arg<EnumValue>(args, 3).value);
      }
    } else if (endpoint == "leaderboard-write") {
      auto metric = call.ends_with("SupplementaryMetric");
      const auto& bytes = arg<Blob>(args, metric ? 3 : 2);
      require(bytes.size() <= 2048 && bytes.size() == arg<std::uint32_t>(args, metric ? 4 : 3));
      fields = {{"api_name", name(args)},
                {"score", std::to_string(arg<std::int64_t>(args, 1))},
                {"extra_data_base64", encode(bytes)}};
      if (metric)
        fields.emplace_back("supplementary_metric", std::to_string(arg<std::int64_t>(args, 2)));
      if (arg<bool>(args, metric ? 5 : 4))
        fields.emplace_back("force_update", "true");
    } else if (endpoint == "achievement-write") {
      fields = {{"api_name", name(args)}};
      if (call.ends_with("Unlock"))
        fields.emplace_back("force_unlock", "true");
      else if (call.ends_with("AddCount")) {
        // Meta's AddCount builder 0x159ce0 clamps uint64 to INT64_MAX.
        fields.emplace_back("add_count", std::to_string(std::min(arg<std::uint64_t>(args, 1),
                                                                 std::uint64_t{INT64_MAX})));
      } else {
        const auto& bits = arg<std::string>(args, 1);
        require(!bits.empty() && bits.find_first_not_of("01") == bits.npos);
        fields.emplace_back("add_bits", bits);
      }
    } else {
      fields = {{"fields", std::string(endpoint == "achievement-definitions" ? definition_fields
                                                                             : progress_fields)}};
      if (call.ends_with("ByName")) {
        const auto& names = arg<std::vector<std::string>>(args, 0);
        require(arg<std::int32_t>(args, 1) >= 0 &&
                names.size() == static_cast<std::size_t>(arg<std::int32_t>(args, 1)));
        fields.emplace_back("api_names", Json(names).dump());
      }
    }
  } catch (...) {
    return {failure(ErrorCode::invalid_argument)};
  }
  auto response = provider.graph_request({std::string(endpoint), fields, {}, {}, {}, page, policy.retry_reads}, stop);
  if (auto error = http_response_error(response))
    return {std::move(error)};
  std::string_view stage = "json";
  try {
    Result result;
    result.model = parse(Json::parse(response.body), kind, stage);
    std::size_t zero_user_ids = 0;
    for (const auto& entry : result.model.elements) {
      if (auto it = entry.fields.find("User"); it != entry.fields.end()) {
        const auto& nested = *std::get<std::shared_ptr<const Model>>(it->second);
        zero_user_ids += std::get<Id>(nested.fields.at("ID")) == 0;
      }
    }
    log::info("ovrplatform", "Progression {} completed payload={} elements={} zero_user_ids={}",
              op.name, kind, result.model.elements.size(), zero_user_ids);
    if (kind == "LeaderboardUpdateStatus")
      log::info("ovrplatform", "Progression {} write did_update={}", op.name,
                std::get<bool>(result.model.fields.at("DidUpdate")));
    return result;
  } catch (...) {
    log::warn("ovrplatform", "Progression {} rejected payload={} stage={} http={}", op.name, kind,
              stage, response.status);
    auto error = failure(ErrorCode::invalid_response);
    error.http_status = response.status;
    return {std::move(error)};
  }
}
} // namespace
void register_progression_endpoints(HttpTransport& http) {
  http.register_endpoint(
      {"leaderboards", HttpMethod::get, "/{app}/leaderboards", {"api_name", "fields"}});
  http.register_endpoint(
      {"leaderboard-entries",
       HttpMethod::get,
       "/leaderboard_entries",
       {"api_name", "fields", "limit", "summary", "filter", "start_at", "offset", "user_ids"}});
  http.register_endpoint(
      {"leaderboard-write",
       HttpMethod::post,
       "/leaderboard_submit_entry",
       {"api_name", "score", "extra_data_base64", "force_update", "supplementary_metric"}});
  http.register_endpoint({"achievement-definitions",
                          HttpMethod::get,
                          "/application/achievement_definitions",
                          {"fields", "api_names"}});
  http.register_endpoint(
      {"achievement-progress", HttpMethod::get, "/me/achievements", {"fields", "api_names"}});
  http.register_endpoint({"achievement-write",
                          HttpMethod::post,
                          "/me/achievements",
                          {"api_name", "force_unlock", "add_count", "add_bits"}});
}
void register_progression_handlers(Session& session, ProgressionPolicy policy) {
  for (const auto& op : operations)
    session.register_handler(
        op.name, {[op, policy](Provider& p, const RequestArgs& args, std::stop_token stop) {
                    return execute(p, op, args, policy, stop);
                  },
                  op.kind});
}
} // namespace venture::ovrplatform
