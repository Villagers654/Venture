#pragma once
#include <array>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include "core/a32_types.h"
#include "core/hle.h"

namespace venture::hle::a32 {
struct Check { bool compatible; std::string_view reason; };
using Checker = Check (*)(const Descriptor&);
// Records a marshaler's alternate, retaining the original A64 Handler exactly.
// Static GL tables call this before process ISA selection, hence no ISA check here.
Handler bind_handler(Handler a64, Handler a32, Checker);
void prepare(FunctionInfo&); // called once, only at A32 registration
void diagnostic(CallContext&);
void trace_enter(CallContext&);
void trace_leave(CallContext&);
void fail(CallContext&, int error);
void read_bytes(GuestAddr, void*, std::size_t);
void write_bytes(GuestAddr, const void*, std::size_t);
void check_out(GuestAddr);
std::uint32_t narrow_out(Type, std::uint64_t);
struct AbiError { int error; };
struct Coverage {
  std::string library, symbol;
  bool descriptor_found = false, compatible = false;
  std::string reason;
};
std::vector<Coverage> coverage();
// Explicit exclusions, with a reviewed reason per symbol; never a library wildcard.
std::string_view a64_only_reason(std::string_view library, std::string_view symbol);
// Explicit Phase-2 adapters still require a descriptor. The caller owns ABI,
// layouts and token lifetimes; this never infers compatibility from C++ types.
void register_adapter(std::string_view library, std::string_view symbol, Handler);

template<class T> bool compatible_type(Type t, bool result = false) {
  using U = std::remove_cv_t<std::remove_reference_t<T>>;
  if (needs_adapter(t)) return false;
  if constexpr (std::is_reference_v<T>) return false; // only CallContext& is injected
  if constexpr (std::is_void_v<U>) return t.kind == Kind::Void;
  else if constexpr (std::is_same_v<U, VarArgs32>) return false; // handled by signature checker
  else if constexpr (std::is_same_v<U, VaList32>) return t.kind == Kind::VaList;
  else if constexpr (std::is_same_v<U, float>) return t.kind == Kind::F32;
  else if constexpr (std::is_same_v<U, double>) return t.kind == Kind::F64;
  else if constexpr (std::is_pointer_v<U>) {
    using P = std::remove_cv_t<std::remove_pointer_t<U>>;
    if (is_out(t.kind)) {
      if (result || std::is_const_v<std::remove_pointer_t<U>>) return false;
      if constexpr (std::is_pointer_v<P>) return t.kind == Kind::OutPtr;
      else if constexpr (std::is_integral_v<P>)
        return t.kind != Kind::OutPtr && (sizeof(P) == 4 || sizeof(P) == 8);
      else return false;
    }
    // A record pointer must have a layout adapter; pointers to scalar buffers
    // and void bytes preserve identity. Function pointers never reach the host.
    if constexpr (std::is_void_v<P>) return t.kind == Kind::Ptr;
    else if constexpr (std::is_arithmetic_v<P>) return t.kind == Kind::Ptr && (!t.size || sizeof(P) == t.size);
    else return false;
  } else if constexpr (std::is_integral_v<U> || std::is_enum_v<U>) {
    if (t.kind == Kind::Ptr) return sizeof(U) == 8; // GuestAddr implementations
    if (t.kind == Kind::Void || t.kind == Kind::F32 || t.kind == Kind::F64 ||
        t.kind == Kind::VaList || is_out(t.kind) || t.kind == Kind::Composite) return false;
    auto bytes = is_wide(t.kind) ? 8u : t.kind == Kind::I8 || t.kind == Kind::U8 ? 1u :
        t.kind == Kind::I16 || t.kind == Kind::U16 ? 2u : 4u;
    return sizeof(U) >= bytes;
  } else return false;
}
template<class R, class... A> Check check_signature(R (*)(A...), const Descriptor& d) {
  std::size_t i = 0;
  bool good = compatible_type<R>(d.result, true), varargs = false;
  auto check = [&]<class T>() {
    if constexpr (std::is_same_v<T, CallContext&>) return;
    else if constexpr (std::is_same_v<T, VarArgs32>) { varargs = true; good &= i == d.arguments.size(); }
    else {
      if (i >= d.arguments.size()) good = false;
      else good &= compatible_type<T>(d.arguments[i]);
      ++i;
    }
  };
  (check.template operator()<A>(), ...);
  good &= i == d.arguments.size() && varargs == d.variadic;
  return {good, good ? "compatible" : "host signature or layout requires adapter"};
}

// Stable storage per argument: pointers handed to F never refer to moved tuple
// temporaries. All Out cells are validated before F and before any copy-back.
// Trivial, so a call does not zero all 32 cells (2.5 KiB) up front: read()
// fills each field of a cell it hands out and finish() reads only those.
struct OutCell {
  GuestAddr guest;
  const Type* type;  // the descriptor's argument, alive for the whole call
  alignas(16) std::array<std::byte, 16> host;
  unsigned host_size;
  std::uint32_t narrowed;
};
static_assert(std::is_trivially_default_constructible_v<OutCell>);
class Invocation {
 public:
  explicit Invocation(CallContext& ctx)
      : ctx_(ctx), cursor_(ctx.cpu(), ctx.registers32()), writer_(ctx.cpu(), ctx.registers32()) {}
  template<class T> T read() {
    if constexpr (std::is_same_v<T, CallContext&>) return ctx_;
    else if constexpr (std::is_same_v<T, VarArgs32>) return VarArgs32(cursor_);
    // Old A64 cursor types are rejected at registration; keep their original
    // wrappers instantiable without changing their representation or hot path.
    else if constexpr (std::is_same_v<T, VarArgs>) return VarArgs(ArgCursor(ctx_));
    else if constexpr (std::is_same_v<T, VaList>) return VaList(0);
    else {
      const Type& type = ctx_.info().a32->arguments[index_++];
      auto bits = cursor_.next(type);
      if constexpr (std::is_same_v<T, VaList32>) return VaList32(bits);
      else if constexpr (std::is_same_v<T, float>) return std::bit_cast<float>(static_cast<std::uint32_t>(bits));
      else if constexpr (std::is_same_v<T, double>) return std::bit_cast<double>(bits);
      else if constexpr (std::is_pointer_v<T>) {
        using P = std::remove_cv_t<std::remove_pointer_t<T>>;
        if constexpr ((std::is_integral_v<P> || std::is_pointer_v<P>) && !std::is_const_v<std::remove_pointer_t<T>>) {
          if (is_out(type.kind) && bits) {
            auto& out = outputs_.at(out_count_++);
            out.guest = bits; out.type = &type; out.host_size = sizeof(P);
            check_out(bits);
            std::uint32_t initial = 0; read_bytes(bits, &initial, 4);
            auto wide = extend({type.kind == Kind::OutPtr || type.kind == Kind::OutSize ? Kind::U32 : Kind::I32}, initial);
            std::memcpy(out.host.data(), &wide, sizeof(P));
            return reinterpret_cast<T>(out.host.data());
          }
        }
        return reinterpret_cast<T>(static_cast<std::uintptr_t>(bits));
      } else if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) return static_cast<T>(bits);
      else throw std::logic_error("unadapted A32 argument");
    }
  }
  void finish() {
    for (unsigned i = 0; i < out_count_; ++i) {
      auto& out = outputs_[i]; std::uint64_t bits = 0;
      std::memcpy(&bits, out.host.data(), out.host_size);
      if (out.host_size == 4 && out.type->kind != Kind::OutPtr && out.type->kind != Kind::OutSize)
        bits = static_cast<std::int64_t>(static_cast<std::int32_t>(bits));
      out.narrowed = narrow_out(*out.type, bits); check_out(out.guest);
    }
    for (unsigned i = 0; i < out_count_; ++i) write_bytes(outputs_[i].guest, &outputs_[i].narrowed, 4);
  }
  template<class R> void result(R value) {
    std::uint64_t bits;
    if constexpr (std::is_pointer_v<R>) bits = reinterpret_cast<std::uintptr_t>(value);
    else if constexpr (std::is_same_v<R, float>) bits = std::bit_cast<std::uint32_t>(value);
    else if constexpr (std::is_same_v<R, double>) bits = std::bit_cast<std::uint64_t>(value);
    else if constexpr (std::is_integral_v<R> || std::is_enum_v<R>) bits = static_cast<std::uint64_t>(value);
    else throw std::logic_error("unadapted A32 result");
    writer_.scalar(ctx_.info().a32->result, bits);
  }
 private:
  CallContext& ctx_;
  ArgCursor32 cursor_;
  ResultWriter32 writer_;
  std::size_t index_ = 0;
  std::array<OutCell, 32> outputs_;
  unsigned out_count_ = 0;
};
template<class R, class... A> void invoke(R (*fn)(A...), CallContext& ctx) {
  try {
    trace_enter(ctx);
    Invocation frame(ctx);
    std::tuple<A...> args{frame.template read<A>()...};
    if constexpr (std::is_void_v<R>) { std::apply(fn, args); frame.finish(); }
    else { auto value = std::apply(fn, args); frame.finish(); frame.result(value); }
  } catch (const AbiError& e) { fail(ctx, e.error); }
  trace_leave(ctx);
}
template<auto F> Handler bind(Handler original) {
  return bind_handler(original, [](CallContext& ctx) { invoke(F, ctx); },
                      [](const Descriptor& d) { return check_signature(F, d); });
}
template<auto F> Handler wrap32() {
  return bind<F>([](CallContext& ctx) { invoke(F, ctx); });
}
template<class Sig> Handler bind_dynamic(Handler original, Handler replacement) {
  return bind_handler(original, replacement ? replacement : +[](CallContext& ctx) { invoke(reinterpret_cast<Sig*>(ctx.user()), ctx); },
                      [](const Descriptor& d) { return check_signature(static_cast<Sig*>(nullptr), d); });
}
} // namespace venture::hle::a32
