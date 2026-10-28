#include "cpu/stubs.h"
#include "core/signal_frame.h"
#include <stdexcept>

namespace venture {
StubCode guest_stub(GuestIsa isa, GuestStub kind, std::uint32_t argument, std::uint32_t svc) {
  if (isa == GuestIsa::a32) {
    // ARM ARM A8.8: ARM-mode MOVW/MOVT split imm16 across imm4:imm12.
    const auto mov = [](std::uint32_t base, std::uint32_t imm) {
      return base | ((imm & 0xf000) << 4) | (imm & 0xfff);
    };
    const auto lo = mov(0xe300c000, argument & 0xffff);
    const auto hi = mov(0xe340c000, argument >> 16);
    switch (kind) {
      case GuestStub::hle_call: return {{lo, hi, 0xef000000 | svc, 0xe12fff1e}, 4};
      case GuestStub::return_to_host: return {{0xef000000 | kSvcReturnToHost}, 1};
      // Depth is bounded to 16 bits by call_return_trampoline(). Keep the same
      // eight-byte return-PC test as A64 (MOVW clears the high half of r12).
      case GuestStub::call_return: return {{lo, 0xef000000 | kSvcReturnToHost}, 2};
      case GuestStub::sigreturn: return {{0xe3a07000u | ((argument & 1) ? 173u : 119u), 0xef000000u}, 2};
      default: throw std::invalid_argument("TLSDESC stubs require A64");
    }
  }
  switch (kind) {
    // MOVZ X17 gives in-process instrumentation (Dobby, ShadowHook) a
    // dead scratch register where an entry patch resumes. Keep its overwrite
    // AFTER the 16-byte far-branch patch area: relocation cannot cross SVC,
    // and a branch back must not clobber the live call id in X16.
    case GuestStub::hle_call: return {{0xd2800010u | ((argument & 0xffffu) << 5),
        0xf2a00010u | ((argument >> 16) << 5), 0xd503201fu, 0xd503201fu,
        0xd2800011u, 0xd4000001u | (svc << 5), 0xd65f03c0}, 7};
    case GuestStub::return_to_host: return {{0xd4000001u | (kSvcReturnToHost << 5)}, 1};
    case GuestStub::call_return: return {{0xd2800010u | (argument << 5), 0xd4000001u | (kSvcReturnToHost << 5)}, 2};
    // Linux arm64 rt_sigreturn. Guest unwinders recognize this sequence,
    // including when no ELF unwind table describes the signal restorer.
    case GuestStub::sigreturn: return {{0xd2801168u, 0xd4000001u}, 2};
    case GuestStub::tlsdesc: return {{0xf9400400, 0xd65f03c0}, 2};
    case GuestStub::tlsdesc_weak: return {{0xa9bf0be1, 0xd53bd041, 0xf9400400, 0xcb010000, 0xa8c10be1, 0xd65f03c0}, 6};
  }
  throw std::invalid_argument("unknown guest stub");
}
} // namespace venture
