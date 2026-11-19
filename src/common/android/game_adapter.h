#pragma once
// Turns a downloaded Quest game into the APK installed on a headset
// (docs/design/native-headsets.md 5.2): the plan's replacements, Venture's
// Quest OpenXR layer, its Java classes and the adapted manifest, taken from
// the assets the Venture APK carries, written into a copy of the game's APK.
#include "common/android/apk_rewrite.h"
#include "common/android/native_host.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace venture::android {
// Where the pieces come from inside the Venture APK.
inline constexpr std::string_view kGameAssets = "assets/game/";
// Builds the edit for `game` from `venture_apk`. Assets that are files
// (libraries) are extracted into `work`. Throws when the plan has blockers or
// an asset is missing.
ApkEdit adapt_game(const fs::path& game, const native_host::Plan& plan, const fs::path& venture_apk,
                   const fs::path& work);
// The next free classesN.dex name in an APK with these entries.
std::string next_dex_name(std::span<const std::string> entries);
// A copy of the alias library whose DT_SONAME is `soname`: the stub's own soname
// is a placeholder at least as long as any library name a game uses. Throws
// when the name does not fit.
std::vector<std::uint8_t> named_alias(std::span<const std::uint8_t> stub, std::string_view soname);
}  // namespace venture::android
