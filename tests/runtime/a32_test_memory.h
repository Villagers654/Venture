#pragma once
#include "core/address_space.h"
#include "cpu/kuser.h"
#include <atomic>
#include <cstring>
#include <stdexcept>

namespace venture::test {
// Minimal stand-in for the arena's lock-free region query. No production arena
// policy is emulated here: these tests own one exact low mapping and kuser.
struct A32Memory {
  static constexpr GuestAddr base = 0x31000000;
  static constexpr std::uint64_t size = 0x10000;
  inline static std::array<std::atomic<std::uint32_t>, 16> permissions{};
  inline static std::atomic<bool> kuser{false};
  A32Memory() {
    const auto mapped = AddressSpace::get().map(base, size, kProtRead | kProtWrite | kProtExec,
        kMapPrivate | kMapAnonymous | kMapFixedNoReplace, nullptr, 0, "[A32 CPU fixture]");
    if (mapped != static_cast<std::int64_t>(base)) throw std::runtime_error("cannot map low A32 test memory");
    for (auto& p : permissions) p.store(kProtRead | kProtWrite | kProtExec);
  }
  ~A32Memory() {
    for (auto& p : permissions) p.store(0);
    AddressSpace::get().unmap(base, size);
    if (kuser.exchange(false)) AddressSpace::get().unmap(kKuserAddress, 4096);
  }
  static bool query(GuestAddr p, std::uint64_t n, std::uint32_t prot) noexcept {
    if (!n || n > (1ull << 32) || p > (1ull << 32) - n) return false;
    if (kuser.load() && p >= kKuserAddress && p + n <= kKuserAddress + 4096)
      return !(prot & ~(kProtRead | kProtExec));
    if (p < base || p + n > base + size) return false;
    for (auto page = (p - base) / 4096; page <= (p + n - 1 - base) / 4096; ++page)
      if ((permissions[page].load() & prot) != prot) return false;
    return true;
  }
  void protect(unsigned page, std::uint32_t prot) {
    permissions.at(page).store(prot);
    if (AddressSpace::get().protect(base + 4096 * page, 4096, prot)) throw std::runtime_error("A32 test mprotect failed");
  }
  GuestAddr arm(unsigned offset, std::initializer_list<std::uint32_t> code) {
    std::memcpy(g2h(base + offset), code.begin(), code.size() * 4); return base + offset;
  }
  GuestAddr thumb(unsigned offset, std::initializer_list<std::uint16_t> code) {
    std::memcpy(g2h(base + offset), code.begin(), code.size() * 2); return (base + offset) | 1;
  }
  void install_kuser() {
    if (install_kuser_page()) throw std::runtime_error("cannot install kuser fixture");
    kuser.store(true);
  }
};
} // namespace venture::test
