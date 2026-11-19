// The per-thread native context: the state that crosses between guest A64
// code running directly on an AArch64 host and Venture's host code, for every
// host that runs guest code natively (docs/design/native-headsets.md, 4.1).
//
// Linux arm64 (native-a64 engine): one context per guest thread, for the
// thread's whole life. Its address is at [TPIDR_EL0 - 16] in the guest TLS
// block, so the entry routines find it before any host TLS or libc call.
// run() enters guest code through venture_native_enter(), which records the
// host side here; the routines, the SIGSYS and halt signal handlers and
// request_halt() use the same fields.
//
// Android (the Meta replacement libraries inside a game's own process): guest
// and host are the same bionic thread, so a context is a frame the entry
// routine builds on the caller's stack for the one call; nothing else uses it.
#pragma once

#include <cstddef>
#include <cstdint>

#include "cpu/native_context_offsets.h"

namespace venture {

struct alignas(16) NativeContext {
  // Guest state.
  std::uint64_t x[31];
  std::uint64_t sp;
  std::uint64_t pc;
  std::uint64_t tpidr;
  std::uint32_t nzcv;
  std::uint32_t fpcr;
  std::uint32_t fpsr;
  std::uint32_t kind;  // SVC immediate of the call kind that entered (kSvcHleCall...)
  std::uint64_t q[32][2];
  // Host state at venture_native_enter (Linux).
  std::uint64_t host_tpidr;
  std::uint64_t host_sp;
  std::uint64_t host_x18;
  std::uint64_t host_fpcr;
  std::uint64_t host_x[12];  // x19..x30
  std::uint64_t host_d[8];   // d8..d15
  // Exit and control.
  std::uint32_t exit_reason;    // VENTURE_NATIVE_EXIT_*
  std::uint32_t svc_immediate;  // with VENTURE_NATIVE_EXIT_SVC
  std::uint32_t halt;           // nonzero: leave run() at the next entry or return
  std::uint32_t flags;          // VENTURE_NATIVE_FLAG_*
  void* owner;                  // the host's object for this thread (its Cpu)
  std::uint64_t reserved;
};

// The A64 routines address these offsets. A 32-bit host (the armeabi-v7a
// Platform loader) only fills the context from C++, where owner is narrower.
#if UINTPTR_MAX == UINT64_MAX
static_assert(sizeof(NativeContext) == VENTURE_NCTX_SIZE);
static_assert(offsetof(NativeContext, x) == VENTURE_NCTX_X);
static_assert(offsetof(NativeContext, x) + 16 * 8 == VENTURE_NCTX_X16);
static_assert(offsetof(NativeContext, x) + 18 * 8 == VENTURE_NCTX_X18);
static_assert(offsetof(NativeContext, x) + 19 * 8 == VENTURE_NCTX_X19);
static_assert(offsetof(NativeContext, x) + 20 * 8 == VENTURE_NCTX_X20);
static_assert(offsetof(NativeContext, x) + 29 * 8 == VENTURE_NCTX_X29);
static_assert(offsetof(NativeContext, sp) == VENTURE_NCTX_SP);
static_assert(offsetof(NativeContext, pc) == VENTURE_NCTX_PC);
static_assert(offsetof(NativeContext, tpidr) == VENTURE_NCTX_TPIDR);
static_assert(offsetof(NativeContext, nzcv) == VENTURE_NCTX_NZCV);
static_assert(offsetof(NativeContext, fpcr) == VENTURE_NCTX_FPCR);
static_assert(offsetof(NativeContext, fpsr) == VENTURE_NCTX_FPSR);
static_assert(offsetof(NativeContext, kind) == VENTURE_NCTX_KIND);
static_assert(offsetof(NativeContext, q) == VENTURE_NCTX_Q);
static_assert(offsetof(NativeContext, q) + 8 * 16 == VENTURE_NCTX_Q8);
static_assert(offsetof(NativeContext, host_tpidr) == VENTURE_NCTX_HOST_TPIDR);
static_assert(offsetof(NativeContext, host_sp) == VENTURE_NCTX_HOST_SP);
static_assert(offsetof(NativeContext, host_x18) == VENTURE_NCTX_HOST_X18);
static_assert(offsetof(NativeContext, host_fpcr) == VENTURE_NCTX_HOST_FPCR);
static_assert(offsetof(NativeContext, host_x) == VENTURE_NCTX_HOST_X19);
static_assert(offsetof(NativeContext, host_d) == VENTURE_NCTX_HOST_D8);
static_assert(offsetof(NativeContext, exit_reason) == VENTURE_NCTX_EXIT_REASON);
static_assert(offsetof(NativeContext, svc_immediate) == VENTURE_NCTX_SVC_IMMEDIATE);
static_assert(offsetof(NativeContext, halt) == VENTURE_NCTX_HALT);
static_assert(offsetof(NativeContext, flags) == VENTURE_NCTX_FLAGS);
static_assert(offsetof(NativeContext, owner) == VENTURE_NCTX_OWNER);
#endif

}  // namespace venture
