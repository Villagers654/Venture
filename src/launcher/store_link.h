#pragma once
// Store links of local imports: the Meta store listing an APK that no
// signed-in account owns is, found by its package name (meta::Plan::store_app_id).
// The install keeps its own key; the link only names the listing its name,
// description, publisher and art come from.
#include <functional>
#include <string>
#include <string_view>

namespace venture::launcher::store_link {
// The app id in a Meta store link (meta.com/experiences/<id>,
// oculus.com/experiences/quest/<id>/...) or a bare id: the longest run of digits
// long enough to be an app id. Empty when there is none.
std::string id_from_link(std::string_view text);
// The app id OculusDB's package lookup answered, when its packageName is
// `package`; empty for anything else (not found, another package, bad data).
std::string oculusdb_id(std::string_view body, std::string_view package);
// The verification every looked-up candidate passes: the public listing's
// package name (latest_supported_binary.package_name) is the APK's.
bool listing_matches(std::string_view listing_package, std::string_view apk_package);

struct Lookups {
  // a: a signed-in account's library lists a game with this package (its
  // listing names the package, so it needs no further check).
  std::string library;
  // b: the online package lookup; empty when the setting is off.
  std::function<std::string(std::string_view package)> online;
  // c: the package name of an app id's public store listing.
  std::function<std::string(std::string_view app_id)> listing_package;
};
struct Resolved {
  std::string id;      // empty: not found
  std::string source;  // "library" or "online"
  // False when a lookup failed (network, server), so the answer is not final
  // and the package may be tried again later.
  bool final = true;
};
// Tries a, then b verified with c.
// Lookups that throw count as failed, not as "not found".
Resolved resolve(std::string_view package, const Lookups &lookups);
}  // namespace venture::launcher::store_link
