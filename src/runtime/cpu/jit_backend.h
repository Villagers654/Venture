#pragma once
// What the runtime's Dynarmic glue needs from the host architecture's backend:
// the JIT state layout handed to callbacks, and the portable block format of
// the persistent block cache (cpu/portable_cache.cpp). One build carries one
// backend, chosen by the host architecture (src/jit/CMakeLists.txt).
#include <array>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#include <dynarmic/backend/x64/a32_jitstate.h>
#include <dynarmic/backend/x64/a64_jitstate.h>
#include <dynarmic/backend/x64/portable_block.h>
#define VENTURE_JIT_PORTABLE_BLOCKS 1
namespace venture::jit_backend {
using A64State = Dynarmic::Backend::X64::A64JitState;
using A32State = Dynarmic::Backend::X64::A32JitState;
namespace portable = Dynarmic::Backend::X64;
inline const std::array<std::uint64_t, 31>& registers(const A64State& state) { return state.reg; }
inline std::uint64_t* registers(A64State& state) { return state.reg.data(); }
inline std::uint32_t* registers(A32State& state) { return state.Reg.data(); }
inline std::uint32_t pstate(const A64State& state) { return state.GetPstate(); }
}  // namespace venture::jit_backend
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <dynarmic/backend/arm64/a32_jitstate.h>
#include <dynarmic/backend/arm64/a64_jitstate.h>
// The arm64 backend has no portable block format yet: no persistent block cache
// and no background translation; blocks are translated when first reached.
#define VENTURE_JIT_PORTABLE_BLOCKS 0
namespace venture::jit_backend {
using A64State = Dynarmic::Backend::Arm64::A64JitState;
using A32State = Dynarmic::Backend::Arm64::A32JitState;
inline const std::array<std::uint64_t, 31>& registers(const A64State& state) { return state.reg; }
inline std::uint64_t* registers(A64State& state) { return state.reg.data(); }
inline std::uint32_t* registers(A32State& state) { return state.regs.data(); }
inline std::uint32_t pstate(const A64State& state) { return state.cpsr_nzcv; }
}  // namespace venture::jit_backend
#else
#error "Venture's JIT has no backend for this host architecture"
#endif
