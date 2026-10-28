#pragma once
#include "core/thread.h"
#include <stdexcept>

namespace venture {
// Untyped A32 host calls are ordered 32-bit words. The ABI packet's typed
// builder supplies aligned pairs/softfp bits through these same word spans.
inline void prepare_guest_call32(Cpu& cpu, GuestAddr fn, const GuestCallArgs& args,
                                 GuestAddr stack_base, std::uint64_t stack_size, GuestAddr trampoline) {
  if (fn > UINT32_MAX || trampoline > UINT32_MAX || !args.doubles.empty())
    throw std::invalid_argument("A32 calls require low addresses and typed softfp arguments");
  for (auto word : args.integers) if (word > UINT32_MAX) throw std::overflow_error("A32 call word exceeds 32 bits");
  for (auto word : args.stack) if (word > UINT32_MAX) throw std::overflow_error("A32 stack word exceeds 32 bits");
  const auto extra = args.integers.size() > 4 ? args.integers.size() - 4 : 0;
  if (extra && !args.stack.empty()) throw std::invalid_argument("explicit A32 stack requires register-only arguments");
  const auto slots = args.stack.empty() ? extra : args.stack.size();
  if (slots > stack_size / 4 || cpu.sp() < 4 * slots) throw std::runtime_error("A32 guest call stack overflow");
  const auto sp = align_down(cpu.sp() - 4 * slots, 8); // AAPCS32 public interface alignment.
  if (sp < stack_base || sp > stack_base + stack_size) throw std::runtime_error("A32 guest call stack overflow");
  auto* out = g2h<std::uint32_t>(sp);
  for (auto word : args.stack) *out++ = static_cast<std::uint32_t>(word);
  for (std::size_t i = 0; i < args.integers.size(); ++i)
    if (i < 4) cpu.set_x(static_cast<int>(i), args.integers[i]); else *out++ = static_cast<std::uint32_t>(args.integers[i]);
  cpu.set_sp(sp); cpu.set_pc(fn); cpu.set_lr(trampoline);
}
} // namespace venture
