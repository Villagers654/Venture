// SPDX-License-Identifier: GPL-3.0-or-later
// The 32-bit Platform loader's calls (docs/design/native-headsets.md, 5.3):
// an ARM32 game's AAPCS32 (softfp) call becomes the A64 register state the
// Platform HLE's handlers read, and their result goes back into r0:r1. The
// handlers stay the ones the 64-bit library runs; only the export's signature,
// generated from the pinned Platform SDK declarations
// (tools/android/platform_exports.py), says how its words map.
//
// Signature: the result letter, then one letter per argument.
//   Result: v void, w a word (x0's low 32 bits), B bool (x0's low byte),
//     I 64 bits (r0:r1), f float (s0, returned in r0), d double (d0, r0:r1),
//     C an ovrKeyValuePair built from the two string arguments, returned
//     through the caller's result pointer (ovrKeyValuePair_makeString; no
//     handler runs).
//   Arguments: i a signed word, u an unsigned word or pointer, I a 64-bit
//     integer, f float, d double; P an array of 32-bit pointers, K an array of
//     ovrKeyValuePair, each counted by the argument after it, and Q an
//     ovrMatchmakingCustomQueryData. P, K and Q are rebuilt in the A64 layout
//     the handlers read (hle/ovrplatform/abi.h) for the length of the call.
//
// Host independent, so the x64 tests drive it with fake memory.
#pragma once
#include <cstdint>
#include <functional>
#include <string_view>

#include "cpu/native_context.h"

namespace venture::ovrplatform::a32_native {
struct Memory {
  // False when the range is not readable/writable.
  std::function<bool(std::uint64_t address, void* out, std::size_t size)> read;
  std::function<bool(std::uint64_t address, const void* data, std::size_t size)> write;
};

// frame[0..3] holds r0-r3 at the call, and the caller's stack arguments start
// at stack. Fills a NativeContext (x[16] = id), calls run with it, and writes
// the result into frame[0] (and frame[1]). False, with frame[0..1] zero, when
// an argument cannot be read; run is not called then.
bool call(std::string_view signature, std::uint32_t id, std::uint32_t* frame, std::uint64_t stack,
          const Memory& memory, const std::function<void(NativeContext&)>& run);
}  // namespace venture::ovrplatform::a32_native
