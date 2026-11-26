#include "hle/ovrplatform/emulated.h"

#include <charconv>
#include <chrono>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <random>

#include "common/http/client.h"
#include "common/log.h"
#include "hle/ovrplatform/session.h"

// The Platform services that have no host counterpart are answered here, as
// the device answers them for a player who is on their own: Meta's room model
// always has the player in a room (ovr_Room_Leave "returns the solo room you
// are now in", OVR_Requests_Room.h), so the solo room is the current room until
// the game makes another. Rooms, parties, matchmaking pools and challenges are
// this session's; the user data store and version 1 cloud storage persist per
// account and app. System dialogs complete as the player closing them. Enum
// values are the SDK's (enum_strings.inc, OVR_*.h, Unity 207 Scripts).
namespace venture::ovrplatform {
namespace {
using Json = nlohmann::json;
using Clock = std::chrono::system_clock;

std::shared_ptr<const Model> share(Model model) { return std::make_shared<const Model>(std::move(model)); }
Model list(std::string kind, std::vector<Model> items = {}) { return {std::move(kind), {}, std::move(items)}; }
Error refused(ErrorCode code, std::string message) {
  auto error = failure(code);
  error.message = std::move(message);
  return error;
}
template <class T>
const T& arg(const RequestArgs& args, std::size_t index) { return std::get<T>(args.values.at(index)); }
const OptionsSnapshot* options_at(const RequestArgs& args, std::size_t index) {
  return index < args.values.size() ? std::get_if<OptionsSnapshot>(&args.values[index]) : nullptr;
}
std::string option(const OptionsSnapshot* options, std::string_view field, std::string fallback = {}) {
  if (!options) return fallback;
  auto it = options->fields.find(field);
  return it == options->fields.end() || it->second.empty() ? fallback : it->second.front();
}
template <class T>
T number(std::string_view text, T fallback) {
  T value{};
  auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size() ? value : fallback;
}
// Session::set_option stores dictionary setters as "<Field>\0<key>" -> {type, value}.
std::map<std::string, std::string, std::less<>> dictionary(const OptionsSnapshot* options, std::string_view field) {
  std::map<std::string, std::string, std::less<>> out;
  if (!options) return out;
  const auto prefix = std::string(field) + '\0';
  for (const auto& [key, values] : options->fields)
    if (key.starts_with(prefix) && values.size() == 2) out[key.substr(prefix.size())] = values[1];
  return out;
}
std::string random_hex(std::size_t digits) {
  static thread_local std::mt19937_64 random{std::random_device{}()};
  constexpr char hex[] = "0123456789abcdef";
  std::string out;
  for (std::size_t i = 0; i < digits; ++i) out += hex[random() & 15];
  return out;
}
std::string uuid() {
  auto text = random_hex(32);
  for (std::size_t at : {8, 13, 18, 23}) text.insert(at, 1, '-');
  return text;
}
// File names for guest-chosen bucket and key names.
std::string encode_name(std::string_view text) {
  constexpr char hex[] = "0123456789abcdef";
  std::string out = "k";
  for (unsigned char c : text) {
    out += hex[c >> 4];
    out += hex[c & 15];
  }
  return out;
}
std::string decode_name(std::string_view text) {
  std::string out;
  if (text.empty() || text[0] != 'k' || text.size() % 2 == 0) return out;
  for (std::size_t i = 1; i + 1 < text.size(); i += 2)
    out += static_cast<char>(std::stoi(std::string(text.substr(i, 2)), nullptr, 16));
  return out;
}
std::int64_t now_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(Clock::now().time_since_epoch()).count();
}
Model data_store(const std::map<std::string, std::string, std::less<>>& values) {
  Model out{"DataStore"};
  for (const auto& [key, value] : values) out.fields.emplace(key, value);
  return out;
}
Model none() { return {}; }

constexpr std::uint32_t room_matchmaking = 1, room_private = 3, room_solo = 4;  // ovrRoomType
constexpr std::uint32_t join_none = 0, join_everyone = 1;                      // ovrRoomJoinPolicy
constexpr std::uint32_t joinability_are_in = 1;                                // ovrRoomJoinability

struct Room {
  Id id = 0;
  std::uint32_t type = room_solo, join_policy = join_none, max_users = 1, version = 1;
  bool locked = false;
  std::string description;
  std::map<std::string, std::string, std::less<>> data;
};
struct Challenge {
  Id id = 0;
  std::string leaderboard, title, description;
  std::int64_t start = 0, end = 0;
  std::uint32_t visibility = 0;
  bool joined = true;
};

class Platform {
 public:
  Platform(AppIdentity app, std::filesystem::path dir) : app_(std::move(app)), dir_(std::move(dir)) {
    std::mt19937_64 random{std::random_device{}()};
    // Locally issued IDs stay clear of Meta's (17-digit) ranges.
    next_id_ = (random() >> 8) | (1ull << 62);
  }
  std::mutex mutex;

  Result me(Provider& provider, std::stop_token stop, Model& out) {
    {
      std::lock_guard lock(mutex);
      if (me_) { out = *me_; return {}; }
    }
    auto r = provider.logged_in_user(stop);
    if (r.error) return r;
    Model user = r.model.kind == "User" ? r.model : user_model(r.user.id, r.user.alias, r.user.display_name);
    std::lock_guard lock(mutex);
    if (!me_) {
      me_ = user;
      my_id_ = r.user.id;
    }
    out = *me_;
    return {};
  }
  Id my_id() const { return my_id_; }
  Id issue() { return ++next_id_; }
  Id app_number() const { return number<Id>(app_.app_id, 0); }
  const AppIdentity& app() const { return app_; }
  const std::filesystem::path& dir() const { return dir_; }

  // Rooms (caller holds mutex).
  Room& current_room() {
    if (!rooms_.contains(current_)) {
      Room solo{issue()};
      current_ = solo.id;
      rooms_.emplace(solo.id, solo);
    }
    return rooms_.at(current_);
  }
  Room& make_room(std::uint32_t type, std::uint32_t policy, std::uint32_t max_users,
                  std::map<std::string, std::string, std::less<>> data) {
    leave_current();
    Room room{issue(), type, policy, max_users ? max_users : 1};
    room.data = std::move(data);
    current_ = room.id;
    return rooms_.emplace(room.id, std::move(room)).first->second;
  }
  Room* room(Id id) {
    auto it = rooms_.find(id);
    return it == rooms_.end() ? nullptr : &it->second;
  }
  void join(Id id) {
    if (id != current_) leave_current();
    current_ = id;
  }
  Room& leave(Id id) {
    if (id == current_) leave_current();
    return current_room();
  }
  Model room_model(const Room& room, const Model& user) const {
    return {"Room",
            {{"ID", room.id}, {"ApplicationID", app_number()}, {"Description", room.description},
             {"DataStore", share(data_store(room.data))}, {"InvitedUsers", share(list("UserArray"))},
             {"IsMembershipLocked", room.locked}, {"JoinPolicy", room.join_policy},
             {"Joinability", joinability_are_in},
             {"MatchedUsers", share(list("MatchmakingEnqueuedUserArray"))}, {"MaxUsers", room.max_users},
             {"Name", std::string{}}, {"Owner", share(user)}, {"Teams", share(list("TeamArray"))},
             {"Type", room.type}, {"Users", share(list("UserArray", {user}))}, {"Version", room.version}}};
  }

  // Party (caller holds mutex).
  std::optional<Id> party;
  std::map<Id, Challenge> challenges;
  std::int64_t next_connection = 1;

  // Persistent state: user data store and version 1 cloud storage.
  Json load_user_data() {
    auto j = Json::parse(read(dir_ / "user-data-store.json").value_or("{}"), nullptr, false);
    return j.is_object() ? j : Json::object();
  }
  bool save_user_data(const Json& j) { return write(dir_ / "user-data-store.json", j.dump()); }
  std::filesystem::path cloud(std::string_view bucket) const {
    return dir_ / "cloud-storage" / encode_name(bucket);
  }
  // Without a state directory (relative paths) nothing is kept on disk.
  static bool write(const std::filesystem::path& path, std::string_view bytes) {
    if (path.is_relative()) return false;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
    auto temp = path;
    temp += ".tmp";
    {
      std::ofstream out(temp, std::ios::binary | std::ios::trunc);
      if (!out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()))) return false;
    }
    std::filesystem::rename(temp, path, error);
    return !error;
  }
  static std::optional<std::string> read(const std::filesystem::path& path) {
    if (path.is_relative()) return {};
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), {});
  }

 private:
  void leave_current() {
    auto it = rooms_.find(current_);
    // The player was the only member, so a room they leave closes.
    if (it != rooms_.end() && it->second.type != room_solo) rooms_.erase(it);
    current_ = 0;
  }
  AppIdentity app_;
  std::filesystem::path dir_;
  std::optional<Model> me_;
  Id my_id_ = 0, next_id_ = 0, current_ = 0;
  std::map<Id, Room> rooms_;
};

using Handler = std::function<Result(Platform&, Provider&, const RequestArgs&, std::stop_token)>;
struct Entry {
  const char* name;
  const char* payload;
  Handler handler;
};
Result answer(Model model) { return {{}, {}, {}, std::move(model)}; }
Result fixed(Model model) { return answer(std::move(model)); }
Handler constant(Model model) {
  return [model = std::move(model)](Platform&, Provider&, const RequestArgs&, std::stop_token) { return fixed(model); };
}
Handler empty_list(const char* kind) { return constant(list(kind)); }
Handler done() { return constant(none()); }

Model report_cancelled() { return {"LaunchReportFlowResult", {{"DidCancel", true}, {"UserReportId", Id{0}}}}; }

Result room_result(Platform& p, Provider& provider, std::stop_token stop,
                   const std::function<Room*(Platform&)>& change) {
  Model user;
  if (auto r = p.me(provider, stop, user); r.error) return r;
  std::lock_guard lock(p.mutex);
  auto* room = change(p);
  if (!room) return {refused(ErrorCode::invalid_argument, "Room not found")};
  return answer(p.room_model(*room, user));
}
Handler room_change(std::function<Room*(Platform&, const RequestArgs&)> change) {
  return [change = std::move(change)](Platform& p, Provider& provider, const RequestArgs& args, std::stop_token stop) {
    return room_result(p, provider, stop, [&](Platform& platform) { return change(platform, args); });
  };
}
Model enqueue_result(std::string pool) {
  return {"MatchmakingEnqueueResult",
          {{"AdminSnapshot", std::shared_ptr<const Model>{}}, {"AverageWait", 0u},
           {"MatchesInLastHourCount", 0u}, {"MaxExpectedWait", 0u}, {"Pool", std::move(pool)},
           {"RecentMatchPercentage", 0u}, {"RequestHash", random_hex(32)}}};
}
Handler enqueue() {
  return [](Platform& p, Provider&, const RequestArgs& args, std::stop_token) {
    const auto* pool = args.values.empty() ? nullptr : std::get_if<std::string>(&args.values[0]);
    return answer(enqueue_result(pool ? *pool : std::string{}));
  };
}
// Matchmaking_CreateRoom/CreateAndEnqueueRoom (pool, maxUsers, ...) and their
// 2 forms (pool, MatchmakingOptions).
Room* matchmaking_room(Platform& p, const RequestArgs& args) {
  const auto* options = options_at(args, 1);
  auto max_users = options ? number<std::uint32_t>(option(options, "CreateRoomMaxUsers"), 0)
                           : arg<std::uint32_t>(args, 1);
  auto policy = number<std::uint32_t>(option(options, "CreateRoomJoinPolicy"), join_everyone);
  return &p.make_room(room_matchmaking, policy, max_users, dictionary(options, "CreateRoomDataStore"));
}
Model party_model(Platform& p, Id id, const Model& user) {
  return {"Party",
          {{"ID", id}, {"InvitedUsers", share(list("UserArray"))}, {"Leader", share(user)},
           {"Room", share(p.room_model(p.current_room(), user))}, {"Users", share(list("UserArray", {user}))}}};
}
Result party_result(Platform& p, Provider& provider, std::stop_token stop, std::optional<Id> wanted) {
  Model user;
  if (auto r = p.me(provider, stop, user); r.error) return r;
  std::lock_guard lock(p.mutex);
  if (!p.party || (wanted && *wanted != *p.party))
    return {refused(ErrorCode::invalid_argument, "Not in a party")};
  return answer(party_model(p, *p.party, user));
}
Model challenge_model(const Challenge& c, const Model& user) {
  return {"Challenge",
          {{"ID", c.id}, {"CreationType", 1u /* ovrChallengeCreationType_UserCreated */},
           {"Description", c.description}, {"Title", c.title}, {"StartDate", c.start}, {"EndDate", c.end},
           {"Visibility", c.visibility}, {"InvitedUsers", share(list("UserArray"))},
           {"Participants", share(c.joined ? list("UserArray", {user}) : list("UserArray"))},
           {"Leaderboard", share(Model{"Leaderboard", {{"ApiName", c.leaderboard}, {"ID", Id{0}}}})}}};
}
Handler challenge(std::function<bool(Challenge&, const RequestArgs&)> change) {
  return [change = std::move(change)](Platform& p, Provider& provider, const RequestArgs& args, std::stop_token stop) {
    Model user;
    if (auto r = p.me(provider, stop, user); r.error) return r;
    std::lock_guard lock(p.mutex);
    auto it = p.challenges.find(arg<Id>(args, 0));
    if (it == p.challenges.end() || !change(it->second, args))
      return Result{refused(ErrorCode::invalid_argument, "Challenge not found")};
    return answer(challenge_model(it->second, user));
  };
}
void apply_challenge_options(Challenge& c, const OptionsSnapshot* options) {
  c.title = option(options, "Title", c.title);
  c.description = option(options, "Description", c.description);
  c.start = number<std::int64_t>(option(options, "StartDate"), c.start);
  c.end = number<std::int64_t>(option(options, "EndDate"), c.end);
  c.visibility = number<std::uint32_t>(option(options, "Visibility"), c.visibility);
}

// Version 1 cloud storage: <dir>/cloud-storage/<bucket>/<key>.data and .json.
Model cloud_metadata(Platform& p, const std::string& bucket, const std::string& key) {
  auto meta = Json::parse(Platform::read(p.cloud(bucket) / (encode_name(key) + ".json")).value_or("{}"), nullptr, false);
  if (!meta.is_object()) meta = Json::object();
  const auto extra = meta.value("extra_data", std::string{});
  return {"CloudStorageMetadata",
          {{"Bucket", bucket}, {"Key", key}, {"Counter", meta.value("counter", std::int64_t{0})},
           {"DataSize", meta.value("data_size", 0u)}, {"SaveTime", meta.value("save_time", std::int64_t{0})},
           {"ExtraData", Blob(reinterpret_cast<const std::byte*>(extra.data()),
                              reinterpret_cast<const std::byte*>(extra.data()) + extra.size())},
           {"Status", 1u /* ovrCloudStorageDataStatus_InSync */},
           {"VersionHandle", std::shared_ptr<const Model>{}}}};
}
Model cloud_update(std::string bucket, std::string key) {
  return {"CloudStorageUpdateResponse",
          {{"Bucket", std::move(bucket)}, {"Key", std::move(key)}, {"Status", 1u /* ovrCloudStorageUpdateStatus_Ok */},
           {"VersionHandle", std::shared_ptr<const Model>{}}}};
}
bool cloud_exists(Platform& p, const std::string& bucket, const std::string& key) {
  std::error_code error;
  return std::filesystem::exists(p.cloud(bucket) / (encode_name(key) + ".data"), error);
}

// UserDataStore: {"private"|"public": {"<user id>": {key: value}}}.
Handler user_data(bool is_public, std::string_view op) {
  return [scope = is_public ? "public" : "private", op = std::string(op)](
             Platform& p, Provider&, const RequestArgs& args, std::stop_token) {
    const auto user = std::to_string(arg<Id>(args, 0));
    std::lock_guard lock(p.mutex);
    auto j = p.load_user_data();
    auto& entries = j[scope][user];
    if (!entries.is_object()) entries = Json::object();
    if (op == "write" || op == "delete") {
      if (op == "write") entries[arg<std::string>(args, 1)] = arg<std::string>(args, 2);
      else entries.erase(arg<std::string>(args, 1));
      if (!p.save_user_data(j)) return Result{refused(ErrorCode::unavailable, "The user data store could not be saved")};
      return answer({"UserDataStoreUpdateResponse", {{"Success", true}}});
    }
    std::map<std::string, std::string, std::less<>> values;
    for (const auto& [key, value] : entries.items())
      if (value.is_string() && (op == "all" || key == arg<std::string>(args, 1))) values[key] = value.get<std::string>();
    return answer(data_store(values));
  };
}
Result http_answer(const http::Response& response) {
  if (response.status < 200 || response.status > 299) {
    auto error = refused(ErrorCode::unavailable, "HTTP " + std::to_string(response.status));
    error.http_status = static_cast<std::int32_t>(response.status);
    return {error};
  }
  return answer({"String", {{"Value", response.body}}});
}
Handler guest_http(bool post) {
  return [post](Platform&, Provider&, const RequestArgs& args, std::stop_token stop) {
    try {
      const auto& url = arg<std::string>(args, 0);
      return http_answer(post ? http::post_guest(url, stop) : http::get_guest(url, stop));
    } catch (const std::exception&) {
      return Result{failure(stop.stop_requested() ? ErrorCode::cancelled : ErrorCode::unavailable)};
    }
  };
}

std::vector<Entry> entries() {
  std::vector<Entry> e;
  const auto add = [&](const char* name, const char* payload, Handler handler) {
    e.push_back({name, payload, std::move(handler)});
  };
  // System dialogs: the player closed them.
  for (auto name : {"ovr_AbuseReport_LaunchAdvancedReportFlow", "ovr_User_LaunchReportFlow",
                    "ovr_User_LaunchReportFlow2", "ovr_User_StopRecordingAndLaunchReportFlow",
                    "ovr_User_StopRecordingAndLaunchReportFlow2"})
    add(name, "LaunchReportFlowResult", constant(report_cancelled()));
  add("ovr_User_LaunchBlockFlow", "LaunchBlockFlowResult",
      constant({"LaunchBlockFlowResult", {{"DidBlock", false}, {"DidCancel", true}}}));
  add("ovr_User_LaunchUnblockFlow", "LaunchUnblockFlowResult",
      constant({"LaunchUnblockFlowResult", {{"DidCancel", true}, {"DidUnblock", false}}}));
  add("ovr_User_LaunchFriendRequestFlow", "LaunchFriendRequestFlowResult",
      constant({"LaunchFriendRequestFlowResult", {{"DidCancel", true}, {"DidSendRequest", false}}}));
  add("ovr_User_StartRecordingForReportFlow", "AbuseReportRecording",
      [](Platform&, Provider&, const RequestArgs&, std::stop_token) {
        return answer({"AbuseReportRecording", {{"RecordingUuid", uuid()}}});
      });
  add("ovr_User_CancelRecordingForReportFlow", "", done());
  add("ovr_Avatar_LaunchAvatarEditor", "AvatarEditorResult", constant({"AvatarEditorResult", {{"RequestSent", false}}}));
  add("ovr_Avatar_UpdateMetaData", "", done());
  add("ovr_Media_ShareToFacebook", "ShareMediaResult",
      constant({"ShareMediaResult", {{"Status", 2u /* ovrShareMediaStatus_Canceled */}}}));
  add("ovr_Room_LaunchInvitableUserFlow", "", done());
  add("ovr_Cowatching_LaunchInviteDialog", "", done());

  // The player's social graph beyond friends: nothing blocked, linked or met.
  add("ovr_User_GetBlockedUsers", "BlockedUserArray", empty_list("BlockedUserArray"));
  add("ovr_User_GetNextBlockedUserArrayPage", "BlockedUserArray", empty_list("BlockedUserArray"));
  add("ovr_User_GetLinkedAccounts", "LinkedAccountArray", empty_list("LinkedAccountArray"));
  add("ovr_User_GetLoggedInUserFriendsAndRooms", "UserAndRoomArray", empty_list("UserAndRoomArray"));
  add("ovr_User_GetLoggedInUserRecentlyMetUsersAndRooms", "UserAndRoomArray", empty_list("UserAndRoomArray"));
  add("ovr_User_GetNextUserAndRoomArrayPage", "UserAndRoomArray", empty_list("UserAndRoomArray"));
  add("ovr_User_GetUserCapabilities", "UserCapabilityArray", empty_list("UserCapabilityArray"));
  add("ovr_User_GetNextUserCapabilityArrayPage", "UserCapabilityArray", empty_list("UserCapabilityArray"));
  add("ovr_GroupPresence_GetSentInvites", "ApplicationInviteArray", empty_list("ApplicationInviteArray"));
  add("ovr_Notification_GetRoomInvites", "RoomInviteNotificationArray", empty_list("RoomInviteNotificationArray"));
  add("ovr_Notification_GetNextRoomInviteNotificationArrayPage", "RoomInviteNotificationArray",
      empty_list("RoomInviteNotificationArray"));
  add("ovr_Notification_MarkAsRead", "", done());
  add("ovr_User_GetSdkAccounts", "SdkAccountArray", [](Platform& p, Provider& provider, const RequestArgs&, std::stop_token stop) {
    Model user;
    if (auto r = p.me(provider, stop, user); r.error) return r;
    return answer(list("SdkAccountArray", {{"SdkAccount", {{"AccountType", 1u /* Oculus */}, {"UserId", p.my_id()}}}}));
  });
  // Unity 69 Message.cs reads this request's payload as a User; the account is
  // not a managed (work) account, so it has no managed info.
  add("ovr_User_GetLoggedInUserManagedInfo", "User", [](Platform& p, Provider& provider, const RequestArgs&, std::stop_token stop) {
    Model user;
    if (auto r = p.me(provider, stop, user); r.error) return r;
    return answer(user);
  });
  for (auto name : {"ovr_User_NewTestUser", "ovr_User_NewEntitledTestUser", "ovr_User_NewTestUserFriends",
                    "ovr_User_TestUserCreateDeviceManifest"})
    add(name, "", [](Platform&, Provider&, const RequestArgs&, std::stop_token) {
      return Result{refused(ErrorCode::not_entitled, "Test users are made by the app's developer organization")};
    });

  // This app, installed and current.
  add("ovr_Application_GetInstalledApplications", "InstalledApplicationArray",
      [](Platform& p, Provider&, const RequestArgs&, std::stop_token) {
        const auto& app = p.app();
        return answer(list("InstalledApplicationArray",
                           {{"InstalledApplication",
                             {{"ApplicationId", app.app_id}, {"PackageName", app.package},
                              {"Status", std::string("INSTALLED")}, {"VersionCode", app.version_code},
                              {"VersionName", app.version_name}}}}));
      });
  add("ovr_Application_CheckAppDownloadProgress", "AppDownloadProgressResult",
      constant({"AppDownloadProgressResult",
                {{"DownloadBytes", std::int64_t{0}}, {"DownloadedBytes", std::int64_t{0}},
                 {"StatusCode", 5u /* ovrAppStatus_Installed */}}}));
  for (auto name : {"ovr_Application_StartAppDownload", "ovr_Application_CancelAppDownload",
                    "ovr_Application_InstallAppUpdateAndRelaunch"})
    add(name, "AppDownloadResult", [](Platform&, Provider&, const RequestArgs&, std::stop_token) {
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
      return answer({"AppDownloadResult",
                     {{"AppInstallResult", 8u /* ovrAppInstallResult_NoNewBinariesAvailable */},
                      {"Timestamp", static_cast<std::int64_t>(ms)}}});
    });

  // Rooms.
  add("ovr_Room_GetCurrent", "Room", room_change([](Platform& p, const RequestArgs&) { return &p.current_room(); }));
  add("ovr_Room_GetCurrentForUser", "Room", room_change([](Platform& p, const RequestArgs& a) -> Room* {
        return arg<Id>(a, 0) == p.my_id() ? &p.current_room() : nullptr;
      }));
  add("ovr_Room_Get", "Room", room_change([](Platform& p, const RequestArgs& a) { return p.room(arg<Id>(a, 0)); }));
  add("ovr_Room_CreateAndJoinPrivate", "Room", room_change([](Platform& p, const RequestArgs& a) {
        return &p.make_room(room_private, static_cast<std::uint32_t>(arg<EnumValue>(a, 0).value),
                            arg<std::uint32_t>(a, 1), {});
      }));
  add("ovr_Room_CreateAndJoinPrivate2", "Room", room_change([](Platform& p, const RequestArgs& a) {
        return &p.make_room(room_private, static_cast<std::uint32_t>(arg<EnumValue>(a, 0).value),
                            arg<std::uint32_t>(a, 1), dictionary(options_at(a, 2), "DataStore"));
      }));
  for (auto name : {"ovr_Room_Join", "ovr_Room_Join2", "ovr_Matchmaking_JoinRoom"})
    add(name, "Room", room_change([](Platform& p, const RequestArgs& a) -> Room* {
          auto* room = p.room(arg<Id>(a, 0));
          if (room) p.join(room->id);
          return room;
        }));
  add("ovr_Room_Leave", "Room", room_change([](Platform& p, const RequestArgs& a) -> Room* {
        if (!p.room(arg<Id>(a, 0))) return nullptr;
        return &p.leave(arg<Id>(a, 0));
      }));
  for (auto name : {"ovr_Room_InviteUser", "ovr_Room_KickUser"})
    add(name, "Room", room_change([](Platform& p, const RequestArgs& a) { return p.room(arg<Id>(a, 0)); }));
  add("ovr_Room_SetDescription", "Room", room_change([](Platform& p, const RequestArgs& a) {
        auto* room = p.room(arg<Id>(a, 0));
        if (room) room->description = arg<std::string>(a, 1), ++room->version;
        return room;
      }));
  add("ovr_Room_UpdateDataStore", "Room", room_change([](Platform& p, const RequestArgs& a) {
        auto* room = p.room(arg<Id>(a, 0));
        const auto& pairs = arg<std::vector<std::string>>(a, 1);
        if (room) {
          for (std::size_t i = 0; i + 1 < pairs.size(); i += 2) room->data[pairs[i]] = pairs[i + 1];
          ++room->version;
        }
        return room;
      }));
  add("ovr_Room_UpdateMembershipLockStatus", "Room", room_change([](Platform& p, const RequestArgs& a) {
        auto* room = p.room(arg<Id>(a, 0));
        if (room) room->locked = arg<EnumValue>(a, 1).value == 1, ++room->version;  // ovrRoomMembershipLockStatus_Lock
        return room;
      }));
  add("ovr_Room_UpdatePrivateRoomJoinPolicy", "Room", room_change([](Platform& p, const RequestArgs& a) {
        auto* room = p.room(arg<Id>(a, 0));
        if (room) room->join_policy = static_cast<std::uint32_t>(arg<EnumValue>(a, 1).value), ++room->version;
        return room;
      }));
  add("ovr_Room_UpdateOwner", "", done());
  add("ovr_Room_GetInvitableUsers", "UserArray", empty_list("UserArray"));
  add("ovr_Room_GetInvitableUsers2", "UserArray", empty_list("UserArray"));
  add("ovr_Room_GetModeratedRooms", "RoomArray", empty_list("RoomArray"));
  add("ovr_Room_GetNextRoomArrayPage", "RoomArray", empty_list("RoomArray"));

  // Matchmaking: the player waits in the pool; nobody else is matched.
  for (auto name : {"ovr_Matchmaking_Enqueue", "ovr_Matchmaking_Enqueue2"})
    add(name, "MatchmakingEnqueueResult", enqueue());
  for (auto name : {"ovr_Matchmaking_EnqueueRoom", "ovr_Matchmaking_EnqueueRoom2"})
    add(name, "MatchmakingEnqueueResult", [](Platform&, Provider&, const RequestArgs&, std::stop_token) {
      return answer(enqueue_result({}));
    });
  for (auto name : {"ovr_Matchmaking_Browse", "ovr_Matchmaking_Browse2"})
    add(name, "MatchmakingBrowseResult", [](Platform&, Provider&, const RequestArgs& args, std::stop_token) {
      return answer({"MatchmakingBrowseResult",
                     {{"EnqueueResult", share(enqueue_result(arg<std::string>(args, 0)))},
                      {"Rooms", share(list("MatchmakingRoomArray"))}}});
    });
  for (auto name : {"ovr_Matchmaking_CreateRoom", "ovr_Matchmaking_CreateRoom2"})
    add(name, "Room", room_change(matchmaking_room));
  for (auto name : {"ovr_Matchmaking_CreateAndEnqueueRoom", "ovr_Matchmaking_CreateAndEnqueueRoom2"})
    add(name, "MatchmakingEnqueueResultAndRoom",
        [](Platform& p, Provider& provider, const RequestArgs& args, std::stop_token stop) {
          auto r = room_result(p, provider, stop, [&](Platform& platform) { return matchmaking_room(platform, args); });
          if (r.error) return r;
          return answer({"MatchmakingEnqueueResultAndRoom",
                         {{"MatchmakingEnqueueResult", share(enqueue_result(arg<std::string>(args, 0)))},
                          {"Room", share(std::move(r.model))}}});
        });
  for (auto name : {"ovr_Matchmaking_Cancel", "ovr_Matchmaking_Cancel2", "ovr_Matchmaking_StartMatch",
                    "ovr_Matchmaking_ReportResultInsecure"})
    add(name, "", done());
  add("ovr_Matchmaking_GetStats", "MatchmakingStats",
      constant({"MatchmakingStats",
                {{"DrawCount", 0u}, {"LossCount", 0u}, {"WinCount", 0u}, {"SkillLevel", 0u},
                 {"SkillMean", 0.0}, {"SkillStandardDeviation", 0.0}}}));
  add("ovr_Matchmaking_GetAdminSnapshot", "MatchmakingAdminSnapshot",
      constant({"MatchmakingAdminSnapshot",
                {{"Candidates", share(list("MatchmakingAdminSnapshotCandidateArray"))}, {"MyCurrentThreshold", 0.0}}}));

  // Party: one the player makes and leads.
  add("ovr_Party_Create", "PartyID", [](Platform& p, Provider&, const RequestArgs&, std::stop_token) {
    std::lock_guard lock(p.mutex);
    p.party = p.issue();
    return answer({"PartyID", {{"ID", *p.party}}});
  });
  add("ovr_Party_GetCurrent", "Party", [](Platform& p, Provider& provider, const RequestArgs&, std::stop_token stop) {
    return party_result(p, provider, stop, {});
  });
  add("ovr_Party_GetCurrentForUser", "Party", [](Platform& p, Provider& provider, const RequestArgs& a, std::stop_token stop) {
    if (arg<Id>(a, 0) != p.my_id() && p.my_id()) return Result{refused(ErrorCode::invalid_argument, "Not in a party")};
    return party_result(p, provider, stop, {});
  });
  for (auto name : {"ovr_Party_Get", "ovr_Party_Join"})
    add(name, "Party", [](Platform& p, Provider& provider, const RequestArgs& a, std::stop_token stop) {
      return party_result(p, provider, stop, arg<Id>(a, 0));
    });
  add("ovr_Party_Leave", "", [](Platform& p, Provider&, const RequestArgs& a, std::stop_token) {
    std::lock_guard lock(p.mutex);
    if (p.party && *p.party == arg<Id>(a, 0)) p.party.reset();
    return Result{};
  });
  for (auto name : {"ovr_Party_Invite", "ovr_Party_GatherInApplication"}) add(name, "", done());

  // Challenges.
  add("ovr_Challenges_Create", "Challenge", [](Platform& p, Provider& provider, const RequestArgs& a, std::stop_token stop) {
    Model user;
    if (auto r = p.me(provider, stop, user); r.error) return r;
    std::lock_guard lock(p.mutex);
    Challenge c{p.issue(), arg<std::string>(a, 0)};
    c.start = now_seconds();
    apply_challenge_options(c, options_at(a, 1));
    return answer(challenge_model(p.challenges.emplace(c.id, c).first->second, user));
  });
  add("ovr_Challenges_Get", "Challenge", challenge([](Challenge&, const RequestArgs&) { return true; }));
  add("ovr_Challenges_Join", "Challenge", challenge([](Challenge& c, const RequestArgs&) { return c.joined = true; }));
  for (auto name : {"ovr_Challenges_Leave", "ovr_Challenges_DeclineInvite"})
    add(name, "Challenge", challenge([](Challenge& c, const RequestArgs&) { c.joined = false; return true; }));
  add("ovr_Challenges_UpdateInfo", "Challenge", challenge([](Challenge& c, const RequestArgs& a) {
        apply_challenge_options(c, options_at(a, 1));
        return true;
      }));
  add("ovr_Challenges_Delete", "", [](Platform& p, Provider&, const RequestArgs& a, std::stop_token) {
    std::lock_guard lock(p.mutex);
    return p.challenges.erase(arg<Id>(a, 0)) ? Result{} : Result{refused(ErrorCode::invalid_argument, "Challenge not found")};
  });
  add("ovr_Challenges_GetList", "ChallengeArray", [](Platform& p, Provider& provider, const RequestArgs&, std::stop_token stop) {
    Model user;
    if (auto r = p.me(provider, stop, user); r.error) return r;
    std::lock_guard lock(p.mutex);
    auto out = list("ChallengeArray");
    for (const auto& [id, c] : p.challenges) out.elements.push_back(challenge_model(c, user));
    return answer(std::move(out));
  });
  for (auto name : {"ovr_Challenges_GetNextChallenges", "ovr_Challenges_GetPreviousChallenges"})
    add(name, "ChallengeArray", empty_list("ChallengeArray"));
  for (auto name : {"ovr_Challenges_GetEntries", "ovr_Challenges_GetEntriesAfterRank", "ovr_Challenges_GetEntriesByIds",
                    "ovr_Challenges_GetNextEntries", "ovr_Challenges_GetPreviousEntries"})
    add(name, "ChallengeEntryArray", empty_list("ChallengeEntryArray"));

  // Version 1 cloud storage, kept per account and app.
  add("ovr_CloudStorage_Save", "CloudStorageUpdateResponse", [](Platform& p, Provider&, const RequestArgs& a, std::stop_token) {
    const auto& bucket = arg<std::string>(a, 0);
    const auto& key = arg<std::string>(a, 1);
    const auto& data = arg<std::vector<std::byte>>(a, 2);
    const Json meta{{"counter", arg<std::int64_t>(a, 4)}, {"extra_data", arg<std::string>(a, 5)},
                    {"data_size", data.size()}, {"save_time", now_seconds()}};
    std::lock_guard lock(p.mutex);
    const auto base = p.cloud(bucket) / encode_name(key);
    auto data_path = base, meta_path = base;
    data_path += ".data";
    meta_path += ".json";
    if (!Platform::write(data_path, {reinterpret_cast<const char*>(data.data()), data.size()}) ||
        !Platform::write(meta_path, meta.dump()))
      return Result{refused(ErrorCode::unavailable, "Cloud storage could not be saved")};
    return answer(cloud_update(bucket, key));
  });
  for (auto name : {"ovr_CloudStorage_Load", "ovr_CloudStorage_LoadMetadata", "ovr_CloudStorage_LoadConflictMetadata"})
    add(name, name == std::string_view("ovr_CloudStorage_Load") ? "CloudStorageData"
              : name == std::string_view("ovr_CloudStorage_LoadMetadata") ? "CloudStorageMetadata"
                                                                         : "CloudStorageConflictMetadata",
        [op = std::string(name)](Platform& p, Provider&, const RequestArgs& a, std::stop_token) {
          const auto& bucket = arg<std::string>(a, 0);
          const auto& key = arg<std::string>(a, 1);
          std::lock_guard lock(p.mutex);
          if (!cloud_exists(p, bucket, key)) return Result{refused(ErrorCode::invalid_argument, "No such cloud storage key")};
          if (op == "ovr_CloudStorage_LoadMetadata") return answer(cloud_metadata(p, bucket, key));
          if (op == "ovr_CloudStorage_LoadConflictMetadata") {
            // One copy, so local and remote are the same version.
            auto meta = share(cloud_metadata(p, bucket, key));
            return answer({"CloudStorageConflictMetadata", {{"Local", meta}, {"Remote", meta}}});
          }
          auto bytes = Platform::read(p.cloud(bucket) / (encode_name(key) + ".data"));
          if (!bytes) return Result{refused(ErrorCode::unavailable, "Cloud storage could not be read")};
          return answer({"CloudStorageData",
                         {{"Bucket", bucket}, {"Key", key}, {"DataSize", static_cast<std::uint32_t>(bytes->size())},
                          {"Data", Blob(reinterpret_cast<const std::byte*>(bytes->data()),
                                        reinterpret_cast<const std::byte*>(bytes->data()) + bytes->size())}}});
        });
  add("ovr_CloudStorage_LoadBucketMetadata", "CloudStorageMetadataArray",
      [](Platform& p, Provider&, const RequestArgs& a, std::stop_token) {
        const auto& bucket = arg<std::string>(a, 0);
        std::lock_guard lock(p.mutex);
        auto out = list("CloudStorageMetadataArray");
        std::error_code error;
        for (const auto& file : std::filesystem::directory_iterator(p.cloud(bucket), error))
          if (file.path().extension() == ".data")
            if (auto key = decode_name(file.path().stem().string()); !key.empty())
              out.elements.push_back(cloud_metadata(p, bucket, key));
        return answer(std::move(out));
      });
  add("ovr_CloudStorage_GetNextCloudStorageMetadataArrayPage", "CloudStorageMetadataArray",
      empty_list("CloudStorageMetadataArray"));
  add("ovr_CloudStorage_Delete", "CloudStorageUpdateResponse", [](Platform& p, Provider&, const RequestArgs& a, std::stop_token) {
    const auto& bucket = arg<std::string>(a, 0);
    const auto& key = arg<std::string>(a, 1);
    std::lock_guard lock(p.mutex);
    std::error_code error;
    std::filesystem::remove(p.cloud(bucket) / (encode_name(key) + ".data"), error);
    std::filesystem::remove(p.cloud(bucket) / (encode_name(key) + ".json"), error);
    return answer(cloud_update(bucket, key));
  });
  for (auto name : {"ovr_CloudStorage_ResolveKeepLocal", "ovr_CloudStorage_ResolveKeepRemote"})
    add(name, "CloudStorageUpdateResponse", [](Platform&, Provider&, const RequestArgs& a, std::stop_token) {
      return answer(cloud_update(arg<std::string>(a, 0), arg<std::string>(a, 1)));
    });
  // Versions come only from conflicts, and one copy never conflicts, so no
  // version handle was ever given out.
  add("ovr_CloudStorage_LoadHandle", "CloudStorageData", [](Platform&, Provider&, const RequestArgs&, std::stop_token) {
    return Result{refused(ErrorCode::invalid_argument, "No such cloud storage version")};
  });

  // User data store, kept per account and app.
  add("ovr_UserDataStore_PrivateWriteEntry", "UserDataStoreUpdateResponse", user_data(false, "write"));
  add("ovr_UserDataStore_PublicWriteEntry", "UserDataStoreUpdateResponse", user_data(true, "write"));
  add("ovr_UserDataStore_PrivateDeleteEntryByKey", "UserDataStoreUpdateResponse", user_data(false, "delete"));
  add("ovr_UserDataStore_PublicDeleteEntryByKey", "UserDataStoreUpdateResponse", user_data(true, "delete"));
  add("ovr_UserDataStore_PrivateGetEntries", "DataStore", user_data(false, "all"));
  add("ovr_UserDataStore_PublicGetEntries", "DataStore", user_data(true, "all"));
  add("ovr_UserDataStore_PrivateGetEntryByKey", "DataStore", user_data(false, "key"));
  add("ovr_UserDataStore_PublicGetEntryByKey", "DataStore", user_data(true, "key"));

  // Co-watching, colocation, livestreaming and NetSync: no session to join.
  add("ovr_Cowatching_IsInSession", "CowatchingState", constant({"CowatchingState", {{"InSession", false}}}));
  add("ovr_Cowatching_GetPresenterData", "String", constant({"String", {{"Value", std::string{}}}}));
  add("ovr_Cowatching_GetViewersData", "CowatchViewerArray", empty_list("CowatchViewerArray"));
  add("ovr_Cowatching_GetNextCowatchViewerArrayPage", "CowatchViewerArray", empty_list("CowatchViewerArray"));
  for (auto name : {"ovr_Cowatching_JoinSession", "ovr_Cowatching_LeaveSession", "ovr_Cowatching_RequestToPresent",
                    "ovr_Cowatching_ResignFromPresenting", "ovr_Cowatching_SetPresenterData",
                    "ovr_Cowatching_SetViewerData"})
    add(name, "", done());
  add("ovr_Colocation_GetCurrentMapUuid", "String", constant({"String", {{"Value", std::string{}}}}));
  for (auto name : {"ovr_Colocation_RequestMap", "ovr_Colocation_ShareMap"}) add(name, "", done());
  add("ovr_Livestreaming_IsAllowedForApplication", "LivestreamingApplicationStatus",
      constant({"LivestreamingApplicationStatus", {{"StreamingEnabled", false}}}));
  for (auto name : {"ovr_Livestreaming_StartStream", "ovr_Livestreaming_StartPartyStream"})
    add(name, "LivestreamingStartResult",
        constant({"LivestreamingStartResult", {{"StreamingResult", std::int32_t{0} /* Unknown: not started */}}}));
  for (auto name : {"ovr_Livestreaming_StopStream", "ovr_Livestreaming_StopPartyStream"})
    add(name, "LivestreamingStatus",
        constant({"LivestreamingStatus",
                  {{"CommentsVisible", false}, {"IsPaused", false}, {"LivestreamingEnabled", false},
                   {"LivestreamingType", 0u}, {"MicEnabled", false}}}));
  add("ovr_Livestreaming_UpdateMicStatus", "", done());
  add("ovr_NetSync_Connect", "NetSyncConnection", [](Platform& p, Provider&, const RequestArgs& a, std::stop_token) {
    std::lock_guard lock(p.mutex);
    return answer({"NetSyncConnection",
                   {{"ConnectionId", p.next_connection++}, {"SessionId", Id{0}},
                    {"Status", 2u /* ovrNetSyncConnectionStatus_Disconnected */},
                    {"DisconnectReason", 3u /* ovrNetSyncDisconnectReason_Failed */},
                    {"ZoneId", option(options_at(a, 0), "ZoneId")}}});
  });
  add("ovr_NetSync_Disconnect", "NetSyncConnection", [](Platform&, Provider&, const RequestArgs& a, std::stop_token) {
    return answer({"NetSyncConnection",
                   {{"ConnectionId", arg<std::int64_t>(a, 0)}, {"SessionId", Id{0}},
                    {"Status", 2u}, {"DisconnectReason", 1u /* LocalTerminated */}, {"ZoneId", std::string{}}}});
  });
  add("ovr_NetSync_GetSessions", "NetSyncSessionArray", empty_list("NetSyncSessionArray"));
  for (auto name : {"ovr_NetSync_GetVoipAttenuation", "ovr_NetSync_GetVoipAttenuationDefault"})
    add(name, "NetSyncVoipAttenuationValueArray", empty_list("NetSyncVoipAttenuationValueArray"));
  for (auto name : {"ovr_NetSync_SetVoipAttenuation", "ovr_NetSync_SetVoipAttenuationModel",
                    "ovr_NetSync_SetVoipChannelCfg", "ovr_NetSync_SetVoipGroup",
                    "ovr_NetSync_SetVoipListentoChannels", "ovr_NetSync_SetVoipMicSource",
                    "ovr_NetSync_SetVoipSessionMuted", "ovr_NetSync_SetVoipSpeaktoChannels",
                    "ovr_NetSync_SetVoipStreamMode"})
    add(name, "", done());
  // No peer-to-peer link carries Meta's ping packets, so a ping times out.
  add("ovr_Net_Ping", "PingResult", [](Platform&, Provider&, const RequestArgs& a, std::stop_token) {
    return answer({"PingResult", {{"ID", arg<Id>(a, 0)}, {"PingTimeUsec", Id{0}}, {"IsTimeout", true}}});
  });

  // Device services.
  add("ovr_PushNotification_Register", "PushNotificationResult",
      constant({"PushNotificationResult", {{"Id", std::string{}}}}));
  add("ovr_UserAgeCategory_Report", "", done());
  add("ovr_Voip_GetMicrophoneAvailability", "MicrophoneAvailabilityState",
      constant({"MicrophoneAvailabilityState", {{"MicrophoneAvailable", true}}}));
  add("ovr_Voip_ReportAppVoipSessions", "", done());
  add("ovr_Voip_SetSystemVoipSuppressed", "SystemVoipState", [](Platform&, Provider&, const RequestArgs& a, std::stop_token) {
    // There is no system (party) VoIP, so it is unavailable unless suppressed.
    return answer({"SystemVoipState",
                   {{"MicrophoneMuted", 0u}, {"Status", arg<bool>(a, 0) ? 2u : 1u /* Suppressed : Unavailable */}}});
  });

  // HTTP for the game, without the account's credentials.
  add("ovr_HTTP_Get", "String", guest_http(false));
  add("ovr_HTTP_Post", "String", guest_http(true));
  return e;
}
}  // namespace

void register_emulated_handlers(Session& session, const AppIdentity& app, std::filesystem::path state) {
  auto platform = std::make_shared<Platform>(app, std::move(state));
  for (auto& entry : entries())
    session.register_handler(entry.name, HandlerSpec{
        [platform, handler = std::move(entry.handler)](Provider& provider, const RequestArgs& args,
                                                       std::stop_token stop) {
          return handler(*platform, provider, args, stop);
        },
        entry.payload});
}
}  // namespace venture::ovrplatform
