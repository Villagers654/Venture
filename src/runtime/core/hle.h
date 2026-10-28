// High-level emulation (HLE) of functions the guest imports.
//
// Every host function exposed to guest code gets a *thunk*: 28 bytes of
// ARM64 code in guest memory (a 32-byte slot),
//
//     movz x16, #id_lo
//     movk x16, #id_hi, lsl #16
//     nop
//     nop
//     movz x17, #0
//     svc  #0x5150
//     ret
//
// The guest calls the thunk like any function. Audited handlers run in the
// SVC callback; other handlers exit to the thread loop. Both receive a
// CallContext and resume at RET. Handlers are usually generated with
// hle::wrap<&function>(), which marshals AAPCS64 arguments and results to an
// ordinary C++ signature.
//
// Marshaling rules (AAPCS64, aarch64-linux-android):
//  * integers, enums, pointers (<= 8 bytes): X0..X7, then 8-byte stack slots
//  * float/double: V0..V7 (S/D view), then 8-byte stack slots
//  * unnamed (variadic) arguments follow the same rules: declare a trailing
//    hle::VarArgs parameter and pull them with next<T>()
//  * va_list parameters (passed by reference on AArch64): declare hle::VaList
//  * a leading CallContext& parameter is injected, not read from the guest
//  * results: integers/pointers in X0, float in S0, double in D0
// 32-bit host `long` / `unsigned long` are rejected at compile time. Use
// int64_t / uint64_t / guest_long (which themselves alias long on LP64 hosts).
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>

#include "core/guest.h"
#include "cpu/cpu.h"

namespace venture {
class GuestThread;
}

namespace venture::hle {

namespace a32 { struct Descriptor; }

class CallContext;
// Borrowed only for an audited callback, after the JIT flushes cached state.
// A single tagged, naturally aligned pointer keeps this argument in a host ABI
// register. Zero selects the ordinary Cpu accessor path.
class Registers {
 public:
  Registers() = default;
  explicit Registers(std::uint64_t* values) : bits_(reinterpret_cast<std::uintptr_t>(values)) {}
  explicit Registers(std::uint32_t* values) : bits_(reinterpret_cast<std::uintptr_t>(values) | 1) {}
  std::uint64_t* a64() const {
    return (bits_ & 1) ? nullptr : reinterpret_cast<std::uint64_t*>(bits_);
  }
  std::uint32_t* a32() const {
    return (bits_ & 1) ? reinterpret_cast<std::uint32_t*>(bits_ & ~std::uintptr_t{1}) : nullptr;
  }
 private:
  std::uintptr_t bits_ = 0;
};
static_assert(sizeof(Registers) == sizeof(void*));
using Handler = void (*)(CallContext&);
// Completes an audited fast case, or declines without consuming the call.
// A conditional fast handler returns false without changing guest arguments or
// completing the call. The run loop then invokes the prepared normal handler.
using TryHandler = bool (*)(CallContext&);
using FunctionId = std::uint32_t;

// InJit is integer-only. InJitFp additionally synchronizes the guest FP state
// and supplies the host FP environment. Both require an audited handler: no guest callbacks, context/PC
// changes, signal delivery, unwind, exit or interruptible blocking waits.
// Handlers that change FPCR/CPSR translation modes also need the run loop.
// InJitGuestFp is InJitFp for pure math: inside the A64 JIT the handler keeps
// the guest MXCSR (see guest_fp_environment); A32 and the run loop use InJitFp.
enum class Execution { RunLoop, InJit, InJitFp, InJitGuestFp };
// Set while an InJitGuestFp handler runs inside the A64 JIT. The live MXCSR is
// then the guest's: host math rounds in the guest mode and its exception flags
// are guest FPSR flags. The handler must not use the Cpu FPSR accessors or set
// the FPCR meanwhile; the JIT keeps the live MXCSR, not the saved copy.
inline thread_local bool guest_fp_environment = false;
struct GuestFpScope {
  explicit GuestFpScope(bool guest) noexcept { guest_fp_environment = guest; }
  ~GuestFpScope() { guest_fp_environment = false; }
  GuestFpScope(const GuestFpScope&) = delete;
  GuestFpScope& operator=(const GuestFpScope&) = delete;
};

struct FunctionInfo {
  Handler handler = nullptr;
  void* user = nullptr;          // e.g. the host function pointer for generated GL/VK thunks
  std::string library;           // "libc.so", "libGLESv3.so", ... (diagnostics)
  std::string name;              // symbol name (diagnostics)
  GuestAddr thunk = 0;           // address of the guest-callable stub
  const a32::Descriptor* a32 = nullptr; // per-(library,symbol), appended for aggregate compatibility
  Execution execution = Execution::RunLoop;
  TryHandler try_handler = nullptr;
  FunctionId id = 0;            // immutable after registry publication
};

// Registers a handler and returns its id. Thread-safe. Thunks are allocated
// lazily in executable guest memory.
FunctionId register_function(std::string_view library, std::string_view name, Handler handler,
                             void* user = nullptr, Execution execution = Execution::RunLoop,
                             TryHandler try_handler = nullptr);
const FunctionInfo& function(FunctionId id);
GuestAddr thunk_address(FunctionId id);

// Convenience for dynamically created function pointers (eglGetProcAddress
// results, JNIEnv tables, ...). Returns the thunk address.
GuestAddr make_thunk(std::string_view library, std::string_view name, Handler handler,
                     void* user = nullptr, Execution execution = Execution::RunLoop,
                     TryHandler try_handler = nullptr);

// Address of the `svc #0x5151` trampoline used as LR for host->guest calls.
GuestAddr return_to_host_trampoline();

// Executes handler `id` for the calling guest thread (used by the run loop).
void dispatch(GuestThread& thread, FunctionId id);

class CallContext {
 public:
  CallContext(GuestThread& thread, Cpu& cpu, const FunctionInfo& info, Registers registers = {})
      : thread_(thread), cpu_(cpu), info_(info), registers_(registers) {}

  GuestThread& thread() const { return thread_; }
  Cpu& cpu() const { return cpu_; }
  const FunctionInfo& info() const { return info_; }
  void* user() const { return info_.user; }

  std::uint64_t x(int i) const {
    if (auto* r = registers_.a64(); r && unsigned(i) < 31) return r[i];
    if (auto* r = registers_.a32(); r && unsigned(i) < 16) return r[i];
    return cpu_.x(i);
  }
  GuestAddr sp() const { if (auto* r = registers_.a32()) return r[13]; return cpu_.sp(); }
  std::uint32_t* registers32() const { return registers_.a32(); }
  float s(int i) const { return std::bit_cast<float>(static_cast<std::uint32_t>(cpu_.v(i).lo)); }
  double d(int i) const { return std::bit_cast<double>(cpu_.v(i).lo); }
  std::uint64_t stack_word(int index) const {  // 8-byte slots above SP at the call
    std::uint64_t value;
    std::memcpy(&value, g2h(sp() + 8ull * static_cast<unsigned>(index)), 8);
    return value;
  }

  void return_x(std::uint64_t value) { return_register(0, value); }
  void return_x1(std::uint64_t value) { return_register(1, value); }
  void return_s(float value) { cpu_.set_v(0, Vec128{std::bit_cast<std::uint32_t>(value), 0}); }
  void return_d(double value) { cpu_.set_v(0, Vec128{std::bit_cast<std::uint64_t>(value), 0}); }

 private:
  void return_register(unsigned r, std::uint64_t value) {
    if (auto* values = registers_.a64()) values[r] = value;
    else if (auto* values = registers_.a32()) values[r] = static_cast<std::uint32_t>(value);
    else cpu_.set_x(r, value);
  }
  GuestThread& thread_;
  Cpu& cpu_;
  const FunctionInfo& info_;
  Registers registers_;
};

// ---------------------------------------------------------------------------
// Argument cursor implementing AAPCS64 allocation.

class ArgCursor {
 public:
  explicit ArgCursor(CallContext& ctx) : ctx_(ctx), stack_(ctx.sp()) {}

  std::uint64_t next_gpr() {
    if (ngrn_ < 8) return ctx_.x(ngrn_++);
    return next_stack_word();
  }
  double next_double() {
    if (nsrn_ < 8) return ctx_.d(nsrn_++);
    return std::bit_cast<double>(next_stack_word());
  }
  float next_float() {
    if (nsrn_ < 8) return ctx_.s(nsrn_++);
    return std::bit_cast<float>(static_cast<std::uint32_t>(next_stack_word()));
  }
  // AAPCS64 C.2/C.8: binary128 occupies a full SIMD register or a 16-byte
  // aligned stack slot. It does not consume an integer-register pair.
  Vec128 next_quad() {
    if (nsrn_ < 8) return ctx_.cpu().v(nsrn_++);
    stack_ = align_up(stack_, 16);
    Vec128 value;
    std::memcpy(&value, g2h(stack_), 16);
    stack_ += 16;
    return value;
  }
  // 16-byte integers start at an even register.
  void align_gpr_pair() {
    if (ngrn_ < 8 && (ngrn_ & 1)) ++ngrn_;
  }
  int gprs_used() const { return ngrn_; }

 private:
  std::uint64_t next_stack_word() {
    std::uint64_t value;
    std::memcpy(&value, g2h(stack_), 8);
    stack_ += 8;
    return value;
  }

  CallContext& ctx_;
  int ngrn_ = 0;
  int nsrn_ = 0;
  GuestAddr stack_;
};

// Trailing parameter for C variadic functions.
class VarArgs {
 public:
  explicit VarArgs(ArgCursor cursor) : cursor_(cursor) {}
  Vec128 next_quad() { return cursor_.next_quad(); }
  template <class T>
  T next() {
    if constexpr (std::is_floating_point_v<T>) {
      // Default argument promotion: floats arrive as doubles.
      return static_cast<T>(cursor_.next_double());
    } else if constexpr (std::is_pointer_v<T>) {
      return reinterpret_cast<T>(static_cast<std::uintptr_t>(cursor_.next_gpr()));
    } else {
      return static_cast<T>(cursor_.next_gpr());
    }
  }

 private:
  ArgCursor cursor_;
};

// AAPCS64 va_list (guest layout); a va_list parameter arrives as a pointer.
struct GuestVaListStruct {
  std::uint64_t stack;
  std::uint64_t gr_top;
  std::uint64_t vr_top;
  std::int32_t gr_offs;
  std::int32_t vr_offs;
};
static_assert(sizeof(GuestVaListStruct) == 32);

class VaList {
 public:
  explicit VaList(GuestAddr list) : list_(*g2h<GuestVaListStruct>(list)) {}
  Vec128 next_quad() {
    GuestAddr slot;
    if (list_.vr_offs < 0) {
      slot = list_.vr_top + static_cast<std::int64_t>(list_.vr_offs);
      list_.vr_offs += 16;
    } else {
      slot = align_up(list_.stack, 16);
      list_.stack = slot + 16;
    }
    Vec128 value;
    std::memcpy(&value, g2h(slot), 16);
    return value;
  }
  template <class T>
  T next() {
    if constexpr (std::is_floating_point_v<T>) {
      return static_cast<T>(std::bit_cast<double>(next_vr()));
    } else if constexpr (std::is_pointer_v<T>) {
      return reinterpret_cast<T>(static_cast<std::uintptr_t>(next_gr()));
    } else {
      return static_cast<T>(next_gr());
    }
  }

 private:
  std::uint64_t next_gr() {
    if (list_.gr_offs < 0) {
      GuestAddr slot = list_.gr_top + static_cast<std::int64_t>(list_.gr_offs);
      list_.gr_offs += 8;
      if (list_.gr_offs <= 0) return load(slot);
    }
    return next_stack();
  }
  std::uint64_t next_vr() {
    if (list_.vr_offs < 0) {
      GuestAddr slot = list_.vr_top + static_cast<std::int64_t>(list_.vr_offs);
      list_.vr_offs += 16;
      if (list_.vr_offs <= 0) return load(slot);
    }
    return next_stack();
  }
  std::uint64_t next_stack() {
    std::uint64_t value = load(list_.stack);
    list_.stack += 8;
    return value;
  }
  static std::uint64_t load(GuestAddr address) {
    std::uint64_t value;
    std::memcpy(&value, g2h(address), 8);
    return value;
  }

  GuestVaListStruct list_;  // a private copy, like a callee's va_copy
};

// ---------------------------------------------------------------------------
// hle::wrap<&fn>() — generate a Handler from a C++ function.

namespace detail {

template <class T>
inline constexpr bool kForbiddenLong =
    sizeof(long) != 8 && (std::is_same_v<std::remove_cv_t<T>, long> ||
                         std::is_same_v<std::remove_cv_t<T>, unsigned long>);

template <class T>
T read_arg(ArgCursor& cursor) {
  static_assert(!kForbiddenLong<T>, "use int64_t/uint64_t: long is 32-bit on Windows hosts");
  if constexpr (std::is_same_v<T, double>) {
    return cursor.next_double();
  } else if constexpr (std::is_same_v<T, float>) {
    return cursor.next_float();
  } else if constexpr (std::is_pointer_v<T>) {
    return reinterpret_cast<T>(static_cast<std::uintptr_t>(cursor.next_gpr()));
  } else if constexpr (std::is_same_v<T, VaList>) {
    return VaList(cursor.next_gpr());
  } else if constexpr (std::is_same_v<T, bool>) {
    return (cursor.next_gpr() & 0xff) != 0;
  } else if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) {
    static_assert(sizeof(T) <= 8, "16-byte integers need manual handling");
    // AAPCS64 leaves the upper bits of narrow arguments unspecified: truncate.
    return static_cast<T>(cursor.next_gpr());
  } else {
    static_assert(sizeof(T) == 0, "unsupported HLE argument type (pass structs by pointer)");
  }
}

template <class R>
void write_result(CallContext& ctx, R value) {
  static_assert(!kForbiddenLong<R>, "use int64_t/uint64_t: long is 32-bit on Windows hosts");
  if constexpr (std::is_same_v<R, double>) {
    ctx.return_d(value);
  } else if constexpr (std::is_same_v<R, float>) {
    ctx.return_s(value);
  } else if constexpr (std::is_pointer_v<R>) {
    ctx.return_x(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(value)));
  } else if constexpr (std::is_same_v<R, bool>) {
    ctx.return_x(value ? 1 : 0);
  } else if constexpr (std::is_integral_v<R> && std::is_signed_v<R>) {
    ctx.return_x(static_cast<std::uint64_t>(static_cast<std::int64_t>(value)));
  } else if constexpr (std::is_integral_v<R> || std::is_enum_v<R>) {
    ctx.return_x(static_cast<std::uint64_t>(value));
  } else {
    static_assert(sizeof(R) == 0, "unsupported HLE result type");
  }
}

template <class... A>
struct ArgList {};

template <class R, class... A>
void invoke(R (*fn)(A...), CallContext& ctx) {
  ArgCursor cursor(ctx);
  auto read = [&]<class T>() -> T {
    if constexpr (std::is_same_v<T, CallContext&>) {
      return ctx;
    } else if constexpr (std::is_same_v<T, VarArgs>) {
      return VarArgs(cursor);
    } else {
      return read_arg<T>(cursor);
    }
  };
  // Braced initialization guarantees left-to-right evaluation.
  std::tuple<A...> args{read.template operator()<A>()...};
  if constexpr (std::is_void_v<R>) {
    std::apply(fn, args);
  } else {
    write_result<R>(ctx, std::apply(fn, args));
  }
}

}  // namespace detail

namespace a32 {
template<auto F> Handler bind(Handler);
template<class Sig> Handler bind_dynamic(Handler, Handler);
}
template <auto F>
Handler wrap() {
  return a32::bind<F>([](CallContext& ctx) { detail::invoke(F, ctx); });
}

// For host function pointers known only at runtime (GL/Vulkan/OpenXR entry
// points): register with user = the host pointer and handler = wrap_dynamic<Sig>().
template <class Sig>
Handler wrap_dynamic() {
  return a32::bind_dynamic<Sig>([](CallContext& ctx) { detail::invoke(reinterpret_cast<Sig*>(ctx.user()), ctx); }, nullptr);
}

}  // namespace venture::hle

#include "core/a32_abi.h"
