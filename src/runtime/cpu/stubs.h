#pragma once
#include "cpu/cpu.h"
#include <array>
#include <span>

namespace venture {
enum class GuestStub { hle_call, return_to_host, call_return, sigreturn, tlsdesc, tlsdesc_weak };
struct StubCode {
  std::array<std::uint32_t, 10> words{};
  std::size_t count{};
  std::span<const std::uint32_t> instructions() const { return {words.data(), count}; }
};
StubCode guest_stub(GuestIsa isa, GuestStub kind, std::uint32_t argument = 0,
                    std::uint32_t svc = kSvcHleCall);
// A32 sigreturn argument: bit 0 selects RT; bit 1 folds an interrupted HLE
// leaf in its unwind recipe. Both variants retain the kernel restorer bytes.
} // namespace venture
