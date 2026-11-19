#pragma once
#include <jni.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/hle.h"
#include "core/thread.h"

namespace venture::java {
enum class Type : char {
  Void = 'V',
  Boolean = 'Z',
  Byte = 'B',
  Char = 'C',
  Short = 'S',
  Int = 'I',
  Long = 'J',
  Float = 'F',
  Double = 'D',
  Object = 'L'
};
struct Descriptor {
  std::vector<Type> parameters;
  Type result;
  static Descriptor parse(std::string_view text);  // throws invalid_argument
};
enum class ArgumentForm { Ellipsis, VaList, Array };
std::vector<jvalue> read_arguments(const Descriptor&, hle::ArgCursor&, ArgumentForm);
std::vector<jvalue> read_arguments32(const Descriptor&, hle::a32::ArgCursor32&, ArgumentForm);

// JNI design.md: escaping is over UTF-16 code units, NOT UTF-8 bytes.
std::string mangle(std::u16string_view text);
std::u16string unescape(std::string_view text);
struct NativeName {
  std::u16string owner, method;
  std::optional<std::u16string> parameters;
};
NativeName demangle(std::string_view symbol);
std::string native_name(std::u16string_view owner, std::u16string_view method,
                        std::optional<std::u16string_view> parameters = {});
std::u16string from_modified_utf8(std::string_view);
std::string to_modified_utf8(std::u16string_view);

// Captured by platform/{win32,posix}/java_entry.S. Offsets are asserted there
// by matching this layout; stack points at the first ABI stack argument. gp
// holds the host ABI's integer argument registers (host_gp_registers), fp its
// first eight floating-point ones.
struct HostArguments {
  std::array<std::uint64_t, 8> gp{};
  std::array<std::uint64_t, 8> fp{};
  const std::uint64_t* stack = nullptr;
};
static_assert(offsetof(HostArguments, fp) == 64);
static_assert(offsetof(HostArguments, stack) == 128);
// The host JVM's calling convention: Windows x64, System V AMD64, or AAPCS64
// (Linux AArch64: x0-x7 and v0-v7, independent, each stack argument in an 8-byte slot).
enum class HostAbi { Win64, SysV, Aapcs64 };
constexpr unsigned host_gp_registers(HostAbi abi) { return abi == HostAbi::Aapcs64 ? 8 : 6; }
#if defined(_WIN32)
inline constexpr HostAbi kHostAbi = HostAbi::Win64;
#elif defined(__aarch64__)
inline constexpr HostAbi kHostAbi = HostAbi::Aapcs64;
#else
inline constexpr HostAbi kHostAbi = HostAbi::SysV;
#endif
struct NativeArguments {
  std::vector<std::uint64_t> integers, stack;
  std::vector<double> doubles;
  GuestCallArgs view() const { return {integers, doubles, stack}; }
};
// A @CriticalNative method gets only its parameters: no JNIEnv* or jclass (ART, API 26+).
NativeArguments native_arguments(const Descriptor&, const HostArguments&, HostAbi, GuestAddr guest_env,
                                 bool critical = false);
std::uint64_t native_result(Type, const GuestCallResult&);
}  // namespace venture::java
