// Shared implementation details for the computation half of libc.
#pragma once
#include <string_view>

#include "core/hle.h"
#include "core/linker.h"
#include "core/thread.h"
#include "core/guest_storage.h"
#include "core/process.h"
#include "hle/libc/libc_parts.h"
#include "hle/libc/linux_errors.h"
namespace venture::hle::libc {
inline void bind(HleLibrary& lib, std::string_view name, Handler fn, Execution execution = Execution::RunLoop) {
  lib.add_function(name, make_thunk(lib.soname(), name, fn, nullptr, execution));
}
template <auto F>
void bind(HleLibrary& lib, std::string_view name, Execution execution = Execution::RunLoop) {
  bind(lib, name, wrap<F>(), execution);
}
template<auto A64, auto A32>
void bind_cursors(HleLibrary& lib, std::string_view name, Execution execution = Execution::RunLoop) {
  bind(lib, name, a32::bind<A32>(wrap<A64>()), execution);
}
const char* guest_getenv(const char* name);
const char* getenv32(const char* name);
int setenv32(const char* name, const char* value, int overwrite);
void set_error(int value);
int* error_location();
[[noreturn]] void fatal(std::string_view message);
void* allocate(std::uint64_t size);
void deallocate(void* p);
void* reallocate(void* p, std::uint64_t size);
void* allocate_aligned(std::uint64_t alignment, std::uint64_t size);
std::uint64_t usable_size(const void* p);
[[noreturn]] void throw_memory_range_fault(GuestAddr address, bool write);
inline void check_memory_range32(GuestAddr address, std::uint64_t size, bool write) {
  if (size && (address > UINT32_MAX || size > (1ull << 32) - address))
    throw_memory_range_fault(address > UINT32_MAX ? std::uint32_t(address) : 0, write);
}
inline void check_memory_range(GuestAddr address, std::uint64_t size, bool write) {
  if (!size) return;
  if (size > UINT64_MAX - address) throw_memory_range_fault(address, write);
  if (process_config().isa() == GuestIsa::a32) check_memory_range32(address, size, write);
}
const char* error_string(int error);
int encode_utf8(char* out, std::uint32_t cp);
Vec128 parse_quad(const char* text, char** end);
std::int64_t parse_signed(const char* text, char** end, int base);
std::uint64_t parse_unsigned(const char* text, char** end, int base);
double parse_double(const char* text, char** end);
float parse_float(const char* text, char** end);
Vec128 double_to_quad(double value);
double quad_to_double(Vec128 value);
template<class Cursor> inline constexpr bool is_a32_cursor =
    std::is_same_v<Cursor, a32::VarArgs32> || std::is_same_v<Cursor, a32::VaList32>;
template <class Cursor>
class PrintArgs final : public PrintfArgs {
 public:
  explicit PrintArgs(Cursor cursor) : cursor_(cursor) {}
  std::uint64_t next_int() override { return cursor_.template next<std::uint64_t>(); }
  double next_double() override { return cursor_.template next<double>(); }
  Vec128 next_quad() override {
    if constexpr (is_a32_cursor<Cursor>) return double_to_quad(cursor_.template next<double>());
    else return cursor_.next_quad();
  }
  std::uint64_t next_integer(PrintfConversion conversion) override {
    if constexpr (is_a32_cursor<Cursor>) {
      if (conversion.width == 8) return cursor_.template next<std::uint64_t>();
      auto word = cursor_.template next<std::uint32_t>();
      return conversion.is_signed ? static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(word))) : word;
    } else return next_int();
  }

 private:
  Cursor cursor_;
};
}  // namespace venture::hle::libc
