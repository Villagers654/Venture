#include "core/a32_unwind.h"
#include "core/low_address_space.h"
#include "platform/memory.h"
#include <bit>
#include <span>

namespace venture {
namespace {
// Arm EHABI32, Index table entries / Frame unwinding instructions:
// https://github.com/ARM-software/abi-aa/blob/main/ehabi32/ehabi32.rst
bool word(GuestAddr address, std::uint32_t& value) {
  return !(address & 3) && low32::query(address, 4, kProtRead) &&
         platform::read_memory(address, &value, 4);
}
std::uint32_t prel31(std::uint32_t base, std::uint32_t value) {
  return base + static_cast<std::uint32_t>(std::bit_cast<std::int32_t>(value << 1) >> 1);
}
const char* execute(std::array<std::uint32_t, 16>& r, std::span<const unsigned char> bytes) {
  bool pc_written = false;
  std::size_t cursor = 0;
  auto next = [&]() -> int { return cursor < bytes.size() ? bytes[cursor++] : -1; };
  auto adjust = [&](std::int64_t delta) {
    auto sp = std::int64_t{r[13]} + delta;
    if (sp < 0 || sp > UINT32_MAX || (sp & 3)) return false;
    r[13] = static_cast<std::uint32_t>(sp); return true;
  };
  auto skip = [&](unsigned n) {
    return low32::query(r[13], n, kProtRead) && adjust(n);
  };
  auto pop = [&](unsigned mask) {
    GuestAddr sp = r[13];
    for (unsigned i = 0; i < 16; ++i) if (mask & (1u << i)) {
      if (!word(sp, r[i])) return false;
      sp += 4;
    }
    // EHABI remark b: a popped r13 replaces vsp after the whole instruction.
    if (!(mask & (1u << 13))) {
      if (sp > UINT32_MAX) return false;
      r[13] = static_cast<std::uint32_t>(sp);
    }
    pc_written |= (mask & (1u << 15)) != 0;
    return !(r[13] & 3);
  };
  while (cursor < bytes.size()) {
    auto op = next();
    if (op < 0x80) {
      auto delta = ((op & 0x3f) << 2) + 4;
      if (!adjust(op & 0x40 ? -delta : delta)) return "invalid stack adjustment";
    } else if (op < 0x90) {
      auto low = next(); if (low < 0) return "truncated register mask";
      auto mask = ((op & 15) << 8) | low;
      if (!mask) return "refuse to unwind";
      if (!pop(mask << 4)) return "unreadable saved core registers";
    } else if (op < 0xa0) {
      auto reg = op & 15;
      if (reg == 13 || reg == 15 || (r[reg] & 3)) return "reserved/invalid vsp register";
      r[13] = r[reg];
    } else if (op < 0xb0) {
      auto mask = ((1u << ((op & 7) + 1)) - 1) << 4;
      if (op & 8) mask |= 1u << 14;
      if (!pop(mask)) return "unreadable saved core registers";
    } else if (op == 0xb0) {
      break;
    } else if (op == 0xb1) {
      auto mask = next();
      if (mask <= 0 || mask > 15) return "reserved/truncated low-register mask";
      if (!pop(mask)) return "unreadable saved core registers";
    } else if (op == 0xb2) {
      std::uint64_t value = 0; unsigned shift = 0;
      for (;;) {
        auto b = next(); if (b < 0 || shift >= 35) return "invalid uleb128 adjustment";
        value |= std::uint64_t(b & 127) << shift; shift += 7;
        if (!(b & 128)) break;
      }
      if (value > (UINT32_MAX - 0x204u) / 4 || !adjust(0x204 + value * 4))
        return "overflowing uleb128 adjustment";
    } else if (op == 0xb3 || op == 0xc8 || op == 0xc9) {
      auto range = next(); if (range < 0) return "truncated VFP register range";
      auto first = (range >> 4) + (op == 0xc8 ? 16 : 0), n = (range & 15) + 1;
      if (first + n > (op == 0xb3 ? 16 : 32)) return "reserved VFP register range";
      if (!skip(n * 8 + (op == 0xb3 ? 4 : 0))) return "unreadable saved VFP registers";
    } else if ((op & 0xf8) == 0xb8 || (op & 0xf8) == 0xd0) {
      if (!skip(((op & 7) + 1) * 8 + (op < 0xc0 ? 4 : 0)))
        return "unreadable saved VFP registers";
    } else {
      return "unsupported/reserved EHABI instruction";
    }
  }
  // An implicit Finish follows the last byte (EHABI short/long formats).
  if (!pc_written) r[15] = r[14];
  return nullptr;
}
}
const char* unwind_a32_frame(std::array<std::uint32_t, 16>& registers,
                             GuestAddr table, std::int32_t entries, bool return_address) {
  if (entries <= 0 || table > UINT32_MAX ||
      !low32::query(table, std::uint64_t(entries) * 8, kProtRead)) return "exidx unavailable";
  auto pc = registers[15] & ~1u;
  if (return_address && pc) --pc; // Attribute a return address to its call site.
  std::uint32_t lo = 0, hi = entries;
  while (lo < hi) {
    auto mid = lo + (hi - lo) / 2; auto address = table + mid * 8ull;
    std::uint32_t offset;
    if (!word(address, offset)) return "unreadable exidx entry";
    if ((prel31(static_cast<std::uint32_t>(address), offset) & ~1u) <= pc) lo = mid + 1;
    else hi = mid;
  }
  if (!lo) return "PC precedes exidx range";
  auto address = table + (lo - 1) * 8ull + 4;
  std::uint32_t header;
  if (!word(address, header)) return "unreadable exidx entry";
  if (header == 1) return "EXIDX_CANTUNWIND";
  const bool inline_entry = header & 0x80000000u;
  if (!inline_entry) {
    address = prel31(static_cast<std::uint32_t>(address), header);
    if (!word(address, header)) return "unreadable extab entry";
  }
  if ((header & 0xff000000u) != 0x80000000u &&
      (header & 0xff000000u) != 0x81000000u &&
      (header & 0xff000000u) != 0x82000000u) return "generic/reserved EHABI personality";
  if (inline_entry && (header >> 24) != 0x80) return "invalid inline EHABI personality";
  std::array<unsigned char, 1022> bytes{}; std::size_t n = 0;
  auto append = [&](std::uint32_t value, unsigned count) {
    for (int i = count - 1; i >= 0; --i) bytes[n++] = static_cast<unsigned char>(value >> (i * 8));
  };
  const auto extra = (header >> 24) == 0x80 ? 0 : (header >> 16) & 255;
  append(header, (header >> 24) == 0x80 ? 3 : 2);
  for (unsigned i = 0; i < extra; ++i) {
    std::uint32_t value;
    if (!word(address + 4ull * (i + 1), value)) return "unreadable extab instructions";
    append(value, 4);
  }
  auto working = registers;
  if (auto error = execute(working, std::span(bytes.data(), n))) return error;
  if (working[15] == registers[15] && working[13] == registers[13]) return "unwind made no progress";
  registers = working; return nullptr;
}
}
