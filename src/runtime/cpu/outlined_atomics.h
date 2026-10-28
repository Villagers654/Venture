// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace venture::outlined_atomics {
// Recognize the complete standard compiler-rt/libgcc AArch64 8-byte ldadd/cas
// body, including its LL/SC fallback. Only ADRP/LDRB relocation bits may vary.
// Instruction encodings implement the published AArch64 ABI; no source copied.
// Reference: compiler-rt/lib/builtins/aarch64/lse.S (LLVM).
enum class Operation { Add, CompareExchange };
enum class Order { Relaxed, Acquire, Release, AcquireRelease };
struct Helper {
  Operation operation;
  Order order;
  std::size_t words;
  constexpr std::size_t index() const {
    return static_cast<unsigned>(operation) * 4 + static_cast<unsigned>(order);
  }
};
inline constexpr std::size_t helper_count = 8;
inline constexpr std::array<const char*, helper_count> names{
  "ldadd8_relax", "ldadd8_acq", "ldadd8_rel", "ldadd8_acq_rel",
  "cas8_relax", "cas8_acq", "cas8_rel", "cas8_acq_rel"};
// The reader returns no value on an inaccessible word. It must not throw guest
// faults during speculative translation-time recognition.
template<class Reader> std::optional<Helper> recognize(Reader&& read) {
  std::size_t base = 0;
  auto first = read(0);
  if (!first) return {};
  if (*first == 0xd503245f) { base = 1; first = read(base); } // BTI c
  if (!first || (*first & 0x9f00001f) != 0x90000010) return {}; // ADRP x16
  auto load = read(base + 1);
  if (!load || (*load & 0xffc003ff) != 0x39400210) return {}; // LDRB w16,[x16,#imm12]
  if (read(base + 2) != std::optional<std::uint32_t>{0x34000070}) return {}; // CBZ w16,+12
  auto atomic = read(base + 3);
  if (!atomic) return {};
  for (unsigned op = 0; op != 2; ++op) for (unsigned order = 0; order != 4; ++order) {
    const bool acquire = (order & 1) != 0, release = (order & 2) != 0;
    const std::uint32_t instruction = op == 0
      ? 0xf8200020u | (acquire ? 0x800000u : 0) | (release ? 0x400000u : 0)
      : 0xc8a07c41u | (acquire ? 0x400000u : 0) | (release ? 0x8000u : 0);
    if (*atomic != instruction) continue;
    const std::array<std::uint32_t, 8> tail = op == 0
      ? std::array<std::uint32_t, 8>{0xd65f03c0, 0xaa0003f0,
          0xc85f7c20u | (acquire ? 0x8000u : 0), 0x8b100011,
          0xc80f7c31u | (release ? 0x8000u : 0), 0x35ffffaf, 0xd65f03c0, 0}
      : std::array<std::uint32_t, 8>{0xd65f03c0, 0xaa0003f0,
          0xc85f7c40u | (acquire ? 0x8000u : 0), 0xeb10001f, 0x54000061,
          0xc8117c41u | (release ? 0x8000u : 0), 0x35ffff91, 0xd65f03c0};
    const auto length = op == 0 ? 7u : 8u;
    for (unsigned i = 0; i < length; ++i)
      if (read(base + 4 + i) != std::optional<std::uint32_t>{tail[i]}) return {};
    return Helper{static_cast<Operation>(op), static_cast<Order>(order), base + 4 + length};
  }
  return {};
}
inline std::optional<Helper> recognize(std::span<const std::uint32_t> words) {
  return recognize([&](std::size_t index) -> std::optional<std::uint32_t> {
    if (index >= words.size()) return {}; return words[index];
  });
}
} // namespace venture::outlined_atomics
