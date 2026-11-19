// SPDX-License-Identifier: GPL-3.0-or-later
// Where the Venture libraries inside an installed game leave their records for
// the Venture app's debug report (docs/design/native-headsets.md, section 9):
// <the game's OBB folder>/venture/, which the game may write and the Venture
// app, as an installer, may read. Each library logs to its own file there; the
// run before is kept as <name>.prev.log. Logging to logcat goes on as well.
#pragma once
#include <jni.h>

#include <filesystem>
#include <string_view>

namespace venture::android_game {
inline constexpr std::string_view kRecordFolder = "venture";

// Context.getObbDir() of the game; empty when it cannot be had.
std::filesystem::path obb_dir(JNIEnv* env, jobject context);
// Starts this library's log at <obb>/venture/<name>.log, once per process.
void start_log(const std::filesystem::path& obb, std::string_view name);
}  // namespace venture::android_game
