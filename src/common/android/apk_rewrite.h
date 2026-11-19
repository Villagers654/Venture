#pragma once
// Writes an adapted copy of a game's APK on the headset
// (docs/design/native-headsets.md 5.2): the game's entries copied as they are
// stored (no recompression, so multi-gigabyte APKs stream), Venture's
// replacements and additions written in, the old signature dropped, and every
// stored entry aligned the way zipalign does (native libraries to 16 KiB
// pages, the rest to 4 bytes). The result is unsigned; the app signs it.
#include "common/paths.h"

#include <cstdint>
#include <map>
#include <stop_token>
#include <string>
#include <vector>

namespace venture::android {
struct ApkEdit {
  // Entries written in, by name, replacing any the game has.
  std::map<std::string, std::vector<std::uint8_t>> bytes;  // small ones (manifest, dex)
  std::map<std::string, fs::path> files;                   // large ones (libraries)
};
// Entries copied from the game: everything but its v1 signature
// (META-INF/MANIFEST.MF, *.SF, *.RSA, *.DSA, *.EC) and what `edit` replaces.
bool kept_from_game(const std::string& name, const ApkEdit& edit);
// Throws on a malformed source, an I/O error, a ZIP64 archive, or when stopped.
void rewrite_apk(const fs::path& source, const ApkEdit& edit, const fs::path& out, std::stop_token stop = {});
// The alignment an entry stored uncompressed gets.
std::uint32_t stored_alignment(const std::string& name);
}  // namespace venture::android
