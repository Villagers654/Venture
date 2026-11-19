#include "cpu/native_entry.h"

#include <atomic>
#include <stdexcept>

namespace venture {
namespace {
constexpr std::uint32_t kSvcs[kNativeCallKinds] = {kSvcHleCall, kSvcHleCallFast, kSvcHleCallFastFp,
                                                   kSvcHleCallFastGuestFp, kSvcEntryHook, kSvcReturnToHost};
constexpr std::uint32_t kNop = 0xd503201fu;
constexpr std::uint32_t kLdrX17Literal8 = 0x58000051u;  // ldr x17, #8
constexpr std::uint32_t kBrX17 = 0xd61f0220u;           // br x17
constexpr std::uint32_t movz_x16(std::uint32_t value) { return 0xd2800010u | ((value & 0xffffu) << 5); }
constexpr std::uint32_t movk_x16_16(std::uint32_t value) { return 0xf2a00010u | ((value >> 16) << 5); }

std::atomic<NativeDispatcher> dispatcher{nullptr};
}  // namespace

std::uint32_t native_call_svc(NativeCall kind) noexcept { return kSvcs[static_cast<std::size_t>(kind)]; }

std::optional<NativeCall> native_call_for_svc(std::uint32_t svc) noexcept {
  for (std::size_t i = 0; i < kNativeCallKinds; ++i)
    if (kSvcs[i] == svc) return static_cast<NativeCall>(i);
  return std::nullopt;
}

StubCode NativeEntry::stub(GuestIsa isa, GuestStub kind, std::uint32_t argument, std::uint32_t svc) const {
  if (isa != GuestIsa::a64) return guest_stub(isa, kind, argument, svc);
  const auto literal = [](StubCode& code, std::uint64_t address) {
    code.words[code.count++] = static_cast<std::uint32_t>(address);
    code.words[code.count++] = static_cast<std::uint32_t>(address >> 32);
  };
  StubCode code;
  switch (kind) {
    case GuestStub::hle_call: {
      const auto call = native_call_for_svc(svc);
      if (!call || *call == NativeCall::return_to_host)
        throw std::invalid_argument("no native entry routine for this HLE call kind");
      code.words = {movz_x16(argument), movk_x16_16(argument), kNop, kNop, kLdrX17Literal8, kBrX17};
      code.count = 6;
      literal(code, routines_[static_cast<std::size_t>(*call)]);
      return code;
    }
    case GuestStub::return_to_host:
      code.words = {kLdrX17Literal8, kBrX17};
      code.count = 2;
      literal(code, routines_[static_cast<std::size_t>(NativeCall::return_to_host)]);
      return code;
    case GuestStub::call_return:
      // The literal stays 8-byte aligned in a 16-byte aligned stub.
      code.words = {movz_x16(argument), kNop, kLdrX17Literal8, kBrX17};
      code.count = 4;
      literal(code, routines_[static_cast<std::size_t>(NativeCall::return_to_host)]);
      return code;
    default:
      return guest_stub(isa, kind, argument, svc);
  }
}

void set_native_dispatcher(NativeDispatcher d) noexcept { dispatcher.store(d, std::memory_order_release); }

extern "C" std::uint32_t venture_native_dispatch(NativeContext* context) noexcept {
  const auto d = dispatcher.load(std::memory_order_acquire);
  if (!d) {
    context->exit_reason = VENTURE_NATIVE_EXIT_HLE_EXCEPTION;
    return VENTURE_NATIVE_EXIT_HLE_EXCEPTION;
  }
  return d(*context);
}

CpuExit NativeContextCpu::run() {
  throw std::logic_error("a NativeContextCpu describes one call; the native-a64 engine runs guest code");
}

void NativeContextCpu::request_halt() {
  std::atomic_ref<std::uint32_t>(context_.halt).store(1, std::memory_order_release);
}

#if defined(__aarch64__)
extern "C" void venture_native_repeat_call();
void NativeContextCpu::rewind_hle_call(GuestAddr thunk) {
  set_x(17, thunk);
  set_pc(reinterpret_cast<std::uint64_t>(&venture_native_repeat_call));
}
#else
void NativeContextCpu::rewind_hle_call(GuestAddr thunk) { set_pc(thunk); }
#endif

#if defined(__aarch64__)
extern "C" {
void venture_native_hle_call();
void venture_native_hle_call_fast();
void venture_native_hle_call_fast_fp();
void venture_native_hle_call_fast_guest_fp();
void venture_native_entry_hook();
void venture_native_return_to_host();
}
const NativeRoutines& native_routines() noexcept {
  static const NativeRoutines routines = {
      reinterpret_cast<std::uint64_t>(&venture_native_hle_call),
      reinterpret_cast<std::uint64_t>(&venture_native_hle_call_fast),
      reinterpret_cast<std::uint64_t>(&venture_native_hle_call_fast_fp),
      reinterpret_cast<std::uint64_t>(&venture_native_hle_call_fast_guest_fp),
      reinterpret_cast<std::uint64_t>(&venture_native_entry_hook),
      reinterpret_cast<std::uint64_t>(&venture_native_return_to_host),
  };
  return routines;
}
const GuestEntry& native_entry() noexcept {
  static const NativeEntry entry(native_routines());
  return entry;
}
#endif

}  // namespace venture
