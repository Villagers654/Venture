#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
namespace venture::platform {
std::array<std::uintptr_t, 4>
make_pipes(); // parent read/write, child read/write
bool valid_pipe(std::uintptr_t);
void close_pipe(std::uintptr_t);
void read_pipe(std::uintptr_t, void *, std::size_t);
void write_pipe(std::uintptr_t, const void *, std::size_t);
} // namespace venture::platform
