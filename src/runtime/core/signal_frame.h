#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include "cpu/cpu.h"
namespace venture { class GuestThread; }

namespace venture::signals {
// Linux arch/arm64/include/uapi/asm/{sigcontext,ucontext}.h and asm-generic/siginfo.h.
struct alignas(8) SigInfo {
  std::int32_t signo{}, error{}, code{}, padding{};
  std::array<std::byte, 112> fields{};
};
struct alignas(16) FpsimdContext {
  std::uint32_t magic = 0x46508001, size = 528, fpsr{}, fpcr{};
  std::array<Vec128, 32> vregs{};
};
struct EsrContext { std::uint32_t magic = 0x45535201, size = 16; std::uint64_t esr{}; };
struct alignas(16) SigContext {
  std::uint64_t fault_address{};
  std::array<std::uint64_t, 31> regs{};
  std::uint64_t sp{}, pc{}, pstate{};
  alignas(16) std::array<std::byte, 4096> reserved{};
};
struct Stack { std::uint64_t sp{}; std::int32_t flags{}, padding{}; std::uint64_t size{}; };
struct alignas(16) UContext {
  std::uint64_t flags{}, link{};
  Stack stack{};
  std::uint64_t sigmask{};
  std::array<std::byte, 120> mask_padding{};
  SigContext mcontext{};
};
struct alignas(16) RtSigFrame { SigInfo info; UContext context; };
struct FrameRecord { std::uint64_t fp{}, lr{}; };
static_assert(sizeof(SigInfo) == 128);
static_assert(sizeof(FpsimdContext) == 528);
static_assert(offsetof(SigContext, reserved) == 288);
static_assert(sizeof(SigContext) == 4384);
static_assert(offsetof(UContext, mcontext) == 176);
static_assert(sizeof(UContext) == 4560);
static_assert(sizeof(RtSigFrame) == 4688);
static_assert(offsetof(RtSigFrame, context) + offsetof(UContext, mcontext) == 304);
// Linux ARM/arm64 compat signal ABI. Legacy sigreturn starts at UContext32;
// rt_sigreturn prefixes the same frame with the 128-byte ILP32 siginfo.
struct SigInfo32 { std::int32_t signo{}, error{}, code{}; std::array<std::byte, 116> fields{}; };
struct SigContext32 {
  std::uint32_t trap{}, error{}, oldmask{};
  std::array<std::uint32_t, 16> regs{};
  std::uint32_t cpsr{}, fault_address{};
};
struct Stack32 { std::uint32_t sp{}; std::int32_t flags{}; std::uint32_t size{}; };
struct alignas(8) UContext32 {
  std::uint32_t flags{}, link{};
  Stack32 stack{};
  SigContext32 mcontext{};
  std::uint64_t sigmask{};
  std::array<std::byte, 120> mask_padding{};
  alignas(8) std::array<std::byte, 512> regspace{};
};
struct SigFrame32 { UContext32 context; std::array<std::uint32_t, 4> retcode{}; };
struct RtSigFrame32 { SigInfo32 info; SigFrame32 sig; };
struct alignas(8) VfpContext32 {
  std::uint32_t magic = 0x56465001, size = 280;
  std::array<std::uint64_t, 32> regs{};
  std::uint32_t fpscr{}, fpexc = 0x40000000, fpinst{}, fpinst2{};
};
static_assert(sizeof(SigInfo32) == 128 && offsetof(SigInfo32, fields) == 12);
static_assert(sizeof(SigContext32) == 84 && offsetof(SigContext32, regs) == 12);
static_assert(offsetof(UContext32, mcontext) == 20 && offsetof(UContext32, sigmask) == 104);
static_assert(offsetof(UContext32, regspace) == 232 && sizeof(UContext32) == 744);
static_assert(sizeof(SigFrame32) == 760 && sizeof(RtSigFrame32) == 888 && sizeof(VfpContext32) == 280);
inline constexpr std::uint32_t kSvcSigreturn = 0x5152;
void return_from_signal(class venture::GuestThread& thread, bool realtime = true);
void forget_thread(class venture::GuestThread& thread);
void interrupt_stopped_threads();
bool restart_last_handler();
}
