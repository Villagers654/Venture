// SPDX-License-Identifier: GPL-3.0-or-later
// The 32-bit Platform loader's dispatcher (armeabi-v7a): an export's AAPCS32
// call, from a32_entry.S, runs the same handler as in the 64-bit library
// through venture_native_dispatch (loader.cpp's NativePlatform), converted by
// the export's generated signature (a32_call.h). An export without one (its
// declaration takes a record the conversion does not rebuild) logs once and
// returns 0, as an unimplemented export does.
#include <cstring>
#include <iterator>
#include <mutex>
#include <set>

#include "android_game/platform/a32_call.h"
#include "common/log.h"
#include "cpu/native_entry.h"
#include "platform/memory.h"

namespace {
const char* const kNames[] = {
#include "export_names.inc"
};
const char* const kSignatures[] = {
#include "export_signatures.inc"
};
static_assert(std::size(kNames) == std::size(kSignatures));

const venture::ovrplatform::a32_native::Memory& memory() {
  static const venture::ovrplatform::a32_native::Memory instance{
      [](std::uint64_t address, void* out, std::size_t size) {
        return venture::platform::read_memory(address, out, size);
      },
      [](std::uint64_t address, const void* data, std::size_t size) {
        // The result record the caller passed: its own writable memory.
        std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(address)), data, size);
        return true;
      },
  };
  return instance;
}

void report(std::uint32_t id, const char* what) {
  static std::mutex mutex;
  static std::set<std::uint32_t> logged;
  std::lock_guard lock(mutex);
  if (logged.insert(id).second) venture::log::warn("ovrplatform", "{} {}; the game gets 0", kNames[id], what);
}
}  // namespace

extern "C" __attribute__((visibility("hidden"))) void venture_platform_a32_call(std::uint32_t* frame,
                                                                                std::uint32_t id) {
  if (id == 0 || id >= std::size(kNames)) {
    frame[0] = frame[1] = 0;
    return;
  }
  const char* signature = kSignatures[id];
  if (!*signature) {
    report(id, "has no 32-bit binding");
    frame[0] = frame[1] = 0;
    return;
  }
  const auto stack = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(frame + 4));
  if (!venture::ovrplatform::a32_native::call(signature, id, frame, stack, memory(),
                                              [](venture::NativeContext& context) { venture_native_dispatch(&context); }))
    report(id, "was called with arguments it cannot read");
}
