#pragma once
// What an Android native library (ELF, 32 or 64 bit, little endian) exports,
// read from its bytes without loading it.
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace venture::android::elf {
// The names of the functions and objects the library defines in its dynamic
// symbol table. Empty for bytes that are not such a library.
std::vector<std::string> exports(std::span<const std::uint8_t> library);
// Its DT_SONAME, or empty.
std::string soname(std::span<const std::uint8_t> library);
}  // namespace venture::android::elf
