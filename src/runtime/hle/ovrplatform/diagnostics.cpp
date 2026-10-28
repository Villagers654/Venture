#include "hle/ovrplatform/diagnostics.h"

#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>

#include "common/log.h"

namespace venture::ovrplatform {
std::string response_fields(const HttpResponse& response) {
  const auto body = nlohmann::json::parse(response.body, nullptr, false);
  if (!body.is_object()) return "<non-object>";
  // Server errors may echo secrets even in property names. Emit only schema
  // field names; count unfamiliar keys without copying their text.
  constexpr std::array<std::string_view, 25> allowed{
      "access_token",   "app_id",       "id",
      "alias",          "user_id",      "account_id",
      "scoped_user_id", "display_name", "user",
      "data",           "error",        "errors",
      "is_valid",       "type",         "code",
      "message",        "expires_at",   "scopes",
      "__warning__",    "messages",     "nonce", "age_category", "org_scoped_id", "paging", "success"};
  std::string result;
  const auto append = [&](const auto& object, std::string_view prefix) {
    unsigned unknown = 0;
    for (const auto& [key, value] : object.items()) {
      if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
        ++unknown;
        continue;
      }
      if (!result.empty()) result += ',';
      result += prefix;
      result += key;
      result += ':';
      result += value.type_name();
      if (key == "nonce" && value.is_string())
        result += ":length=" +
                  std::to_string(value.template get_ref<const std::string&>().size());
    }
    if (unknown) {
      if (!result.empty()) result += ',';
      result += std::string(prefix) + "<unknown>:" + std::to_string(unknown);
    }
  };
  append(body, "");
  for (auto wrapper : {"data", "user", "__warning__"})
    if (body.contains(wrapper) && body[wrapper].is_object())
      append(body[wrapper], std::string(wrapper) + '.');
  const auto id = body.find("id");
  const bool zero = id != body.end() &&
                    ((*id == "0") || (id->is_number_integer() && *id == 0));
  result += zero ? ";id_zero=1" : ";id_zero=0";
  bool checkup = false;
  if (body.contains("__warning__") && body["__warning__"].is_object()) {
    const auto messages = body["__warning__"].find("messages");
    if (messages != body["__warning__"].end() && messages->is_array())
      for (const auto& message : *messages) {
        const auto* value = &message;
        if (message.is_object()) {
          const auto field = message.find("message");
          if (field == message.end()) continue;
          value = &*field;
        }
        if (!value->is_string()) continue;
        const auto& text = value->get_ref<const std::string&>();
        checkup |= (text.find("User ID") != std::string::npos ||
                    text.find("User Profile") != std::string::npos) &&
                   text.find("Data Use Checkup") != std::string::npos &&
                   text.find("requires") != std::string::npos;
      }
  }
  // A fixed category conveys the actionable server warning without echoing
  // its raw text, URLs, identifiers or potentially reflected credentials.
  if (checkup) result += ";data_use_checkup_required=1";
  return result;
}

void log_auth_response(std::string_view path, std::string_view app_id,
                       const HttpResponse& response) {
  log::info("ovrplatform", "Meta {} exchange_app_id={} http={} fields=[{}]",
            path, valid_id(app_id) ? app_id : "<invalid>", response.status,
            response_fields(response));
}
}  // namespace venture::ovrplatform
