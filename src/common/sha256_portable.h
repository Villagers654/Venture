// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// SHA-256 (FIPS 180-4) without a crypto library, for hosts whose system
// offers none to native code (Android: the NDK has no stable crypto API).
// Windows and desktop Linux keep their system providers (digest.h).
#include <array>
#include <cstdint>
#include <span>

namespace venture::digest::portable {
class Sha256 {
 public:
  void update(std::span<const std::uint8_t> bytes);
  std::array<std::uint8_t, 32> finish();

 private:
  void block(const std::uint8_t* data);
  std::array<std::uint32_t, 8> state_{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffered_ = 0;
  std::uint64_t length_ = 0;
};
}  // namespace venture::digest::portable
