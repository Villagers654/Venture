#include "launcher/store_link.h"

#include "common/json.h"
#include <algorithm>

namespace venture::launcher::store_link {
std::string id_from_link(std::string_view text) {
  std::string id;
  for (std::size_t i = 0; i < text.size();) {
    auto end = i;
    while (end < text.size() && text[end] >= '0' && text[end] <= '9') ++end;
    if (end - i >= 5 && end - i > id.size()) id = std::string(text.substr(i, end - i));  // the longest run: a query parameter can't win
    i = std::max(end, i + 1);
  }
  return digits(id, 5) ? id : std::string();
}

std::string oculusdb_id(std::string_view body, std::string_view package) {
  Json j = Json::parse(body, nullptr, false);
  if (j.is_discarded()) return {};
  // One application, or a list of them.
  auto pick = [&](const Json &app) {
    auto id = str(app, "id");
    return str(app, "packageName") == package && digits(id, 5) ? id : std::string();
  };
  if (j.is_object()) return pick(j);
  if (j.is_array())
    for (const auto &app : j)
      if (auto id = pick(app); !id.empty()) return id;
  return {};
}

bool listing_matches(std::string_view listing_package, std::string_view apk_package) {
  return !apk_package.empty() && listing_package == apk_package;
}

Resolved resolve(std::string_view package, const Lookups &lookups) {
  Resolved out;
  if (digits(lookups.library, 5)) {
    out.id = lookups.library;
    out.source = "library";
    return out;
  }
  auto attempt = [&](const std::function<std::string(std::string_view)> &lookup, const char *source) {
    if (!lookup) return false;
    try {
      auto id = lookup(package);
      if (!digits(id, 5)) return false;
      if (!lookups.listing_package || !listing_matches(lookups.listing_package(id), package)) return false;
      out.id = std::move(id);
      out.source = source;
      return true;
    } catch (...) {
      out.final = false;
      return false;
    }
  };
  if (attempt(lookups.online, "online")) out.final = true;
  return out;
}
}  // namespace venture::launcher::store_link
