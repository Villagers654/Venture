#pragma once
namespace venture::hle {
// Call during runtime HLE initialization, before loading the APK's libraries.
void register_libandroid();
void register_libmediandk();
void register_libcamera2ndk();
}  // namespace venture::hle
