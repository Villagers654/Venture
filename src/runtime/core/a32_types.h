// AAPCS32 base PCS (softfp). Metadata is keyed by exported symbol, never F.
#pragma once
#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>
#include "cpu/cpu.h"

namespace venture::hle::a32 {
enum class Kind : std::uint8_t {
  Void, I8, U8, I16, U16, I32, U32, I64, U64, F32, F64,
  Ptr, Size, SSize, Long, ULong, Off, Time, Handle, Callback,
  OutSize, OutLong, OutOff, OutTime, OutPtr, Layout, Composite, VaList, Unsupported
};
struct Type {
  Kind kind = Kind::Void;
  std::string_view tag{};
  std::uint16_t size = 0, alignment = 4;
};
struct Descriptor {
  std::string_view library, symbol;
  Type result;
  std::span<const Type> arguments;
  bool variadic = false;
  std::string_view source;
};
const Descriptor* descriptor(std::string_view library, std::string_view symbol);
std::span<const Descriptor> ndk_descriptors();
std::span<const Descriptor> libc_descriptors(); // later NDK API declarations, separate from base
std::span<const Descriptor> graphics_descriptors();
std::span<const Descriptor> vendor_descriptors();
std::string_view kind_name(Kind);
bool is_signed(Kind);
bool is_wide(Kind);
bool is_out(Kind);
bool needs_adapter(Type);
std::uint64_t extend(Type, std::uint64_t);

// ARM IHI 0042, base PCS Stage C: skipped registers are never backfilled;
// doublewords round NCRN to even and NSAA to 8. Composites alone may split.
class ArgCursor32 {
 public:
  explicit ArgCursor32(Cpu& cpu, bool sret = false)
      : ArgCursor32(cpu, nullptr, sret) {}
  ArgCursor32(Cpu& cpu, const std::uint32_t* registers, bool sret = false)
      : cpu_(&cpu), registers_(registers), reg_(sret ? 1 : 0), stack_(registers ? registers[13] : cpu.sp()) {}
  std::uint32_t word();
  std::uint64_t pair();
  std::uint64_t next(Type);
  void composite(std::span<std::byte> out, unsigned alignment = 4);
  unsigned registers_used() const { return reg_; }
  GuestAddr stack_address() const { return stack_; }
 private:
  Cpu* cpu_;
  const std::uint32_t* registers_;
  unsigned reg_ = 0;
  GuestAddr stack_;
};
class ResultWriter32 {
 public:
  explicit ResultWriter32(Cpu& cpu, std::uint32_t* registers = nullptr)
      : cpu_(cpu), registers_(registers), sret_(registers ? registers[0] : static_cast<std::uint32_t>(cpu.x(0))) {}
  void scalar(Type, std::uint64_t bits);
  void composite(std::span<const std::byte> bytes);
 private:
  Cpu& cpu_;
  std::uint32_t* registers_;
  void word(unsigned reg, std::uint32_t bits) {
    if (registers_) registers_[reg] = bits;
    else cpu_.set_x(reg, bits);
  }
  GuestAddr sret_;
};
class VarArgs32 {
 public:
  explicit VarArgs32(ArgCursor32 cursor) : cursor_(cursor) {}
  template<class T> T next() {
    if constexpr (std::is_floating_point_v<T>) return static_cast<T>(std::bit_cast<double>(cursor_.pair()));
    else if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>(static_cast<std::uintptr_t>(cursor_.word()));
    else if constexpr (sizeof(T) == 8) return static_cast<T>(cursor_.pair());
    else return static_cast<T>(cursor_.word());
  }
 private:
  ArgCursor32 cursor_;
};
// bionic/arch-arm/include/machine/_types.h: __va_list is one pointer word,
// passed by value, pointing at the callee's contiguous saved-register/stack area.
class VaList32 {
 public:
  explicit VaList32(GuestAddr pointer) : pointer_(pointer) {}
  std::uint64_t take(unsigned bytes);
  template<class T> T next() {
    if constexpr (std::is_floating_point_v<T>) return static_cast<T>(std::bit_cast<double>(take(8)));
    else if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>(static_cast<std::uintptr_t>(take(4)));
    else return static_cast<T>(take(sizeof(T) == 8 ? 8 : 4));
  }
 private:
  GuestAddr pointer_;
};

// An ordered host->guest call frame. Never accepts a host pointer implicitly.
class CallBuilder32 {
 public:
  CallBuilder32& add(Type, std::uint64_t bits);
  CallBuilder32& composite(std::span<const std::byte>, unsigned alignment = 4);
  CallBuilder32& pointer(GuestAddr address);
  // Compatibility entry point: these are words, never inferred 64-bit values.
  CallBuilder32& untyped_words(std::span<const std::uint64_t>);
  template<class T> CallBuilder32& value(T v) {
    if constexpr (std::is_same_v<T, float>) return add({Kind::F32}, std::bit_cast<std::uint32_t>(v));
    else if constexpr (std::is_same_v<T, double>) return add({Kind::F64}, std::bit_cast<std::uint64_t>(v));
    else {
      static_assert(std::is_integral_v<T> && sizeof(T) <= 8);
      constexpr auto k = sizeof(T) == 8 ? (std::is_signed_v<T> ? Kind::I64 : Kind::U64) :
        sizeof(T) == 4 ? (std::is_signed_v<T> ? Kind::I32 : Kind::U32) :
        sizeof(T) == 2 ? (std::is_signed_v<T> ? Kind::I16 : Kind::U16) :
                        (std::is_signed_v<T> ? Kind::I8 : Kind::U8);
      return add({k}, static_cast<std::uint64_t>(v));
    }
  }
  void apply(Cpu& cpu, GuestAddr stack_bottom) const;
  std::span<const std::uint32_t> registers() const { return {registers_.data(), reg_}; }
  std::span<const std::uint32_t> stack() const { return stack_; }
 private:
  void word(std::uint32_t);
  std::array<std::uint32_t, 4> registers_{};
  unsigned reg_ = 0;
  std::vector<std::uint32_t> stack_;
};
} // namespace venture::hle::a32
