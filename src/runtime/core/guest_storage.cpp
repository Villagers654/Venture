#include "core/guest_storage.h"
#include "core/address_space.h"
#include "core/low_address_space.h"
#include "core/process.h"
#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace venture {
GuestAddr low_data(std::span<const std::byte> data, std::uint64_t alignment) {
  if (!alignment || (alignment & (alignment - 1)) || alignment > 4096)
    throw std::invalid_argument("invalid low data alignment");
  struct Cache {
    std::mutex mutex;
    std::map<std::pair<std::uint64_t, std::string>, GuestAddr> entries;
  };
  static auto* cache = new Cache;
  std::lock_guard lock(cache->mutex);
  auto key = std::make_pair(alignment, std::string(reinterpret_cast<const char*>(data.data()), data.size()));
  if (auto it = cache->entries.find(key); it != cache->entries.end()) return it->second;
  // Immutable pages enforce the lifetime contract and avoid a writable alias.
  auto p =
      low32::allocate(std::max<std::size_t>(data.size(), 1), kProtRead | kProtWrite, "[HLE immutable]", 4096);
  if (!p) throw std::bad_alloc();
  std::memcpy(g2h(p), data.data(), data.size());
  if (low32::protect(p, std::max<std::size_t>(data.size(), 1), kProtRead)) {
    low32::free(p);
    throw std::bad_alloc();
  }
  cache->entries.emplace(std::move(key), p);
  return p;
}
const char* low_string(std::string_view text) {
  std::string terminated(text);
  terminated.push_back('\0');
  return g2h<const char>(low_data(std::as_bytes(std::span(terminated.data(), terminated.size()))));
}
const char* guest_string(const char* text) {
  return text && process_config().isa() == GuestIsa::a32 ? low_string(text) : text;
}
GuestStorage::~GuestStorage() {
  if (address_) {
    if (low_)
      low32::free(address_);
    else
      AddressSpace::get().free(address_);
  }
}
GuestAddr GuestStorage::resize(std::uint64_t bytes) {
  if (bytes <= capacity_) return address_;
  if (bytes > UINT64_MAX - 4095) throw std::bad_alloc();
  auto p = AddressSpace::get().allocate(bytes, kProtRead | kProtWrite, "[HLE storage]");
  if (!p) throw std::bad_alloc();
  if (address_) {
    std::memcpy(g2h(p), g2h(address_), capacity_);
    if (low_)
      low32::free(address_);
    else
      AddressSpace::get().free(address_);
  }
  low_ = process_config().isa() == GuestIsa::a32;
  address_ = p;
  capacity_ = align_up(bytes, 4096);
  return p;
}
const char* GuestStorage::string(std::string_view text) {
  auto p = resize(text.size() + 1);
  std::memcpy(g2h(p), text.data(), text.size());
  g2h<char>(p)[text.size()] = '\0';
  return g2h<const char>(p);
}
}  // namespace venture
