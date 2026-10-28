#pragma once
#include "core/guest.h"
#include <span>
#include <string_view>

namespace venture {
// A32 immutable data, interned for process lifetime. No host pointer is embedded
// or implicitly narrowed. Adapters must build ILP32 wire layouts explicitly.
GuestAddr low_data(std::span<const std::byte> data, std::uint64_t alignment = 16);
const char* low_string(std::string_view text);
// ABI-specific immutable string result: A64 preserves its existing pointer.
const char* guest_string(const char* text);
// Per-call/per-thread mutable data with explicit ownership, independent of heap.
class GuestStorage {
  GuestAddr address_ = 0;
  std::uint64_t capacity_ = 0;
  bool low_ = false;

 public:
  GuestStorage() = default;
  ~GuestStorage();
  GuestStorage(const GuestStorage&) = delete;
  GuestStorage& operator=(const GuestStorage&) = delete;
  GuestAddr resize(std::uint64_t bytes);
  GuestAddr address() const { return address_; }
  const char* string(std::string_view text);
};
}  // namespace venture
