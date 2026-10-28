// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/digest.h"
#include <array>
#include <stdexcept>
#include <algorithm>
#include <limits>
#include <windows.h>
#include <bcrypt.h>

namespace venture::digest {
namespace {
struct Provider {
  BCRYPT_ALG_HANDLE handle = nullptr;
  Provider() {
    if (BCryptOpenAlgorithmProvider(&handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
      throw std::runtime_error("cannot open SHA-256 provider");
  }
  ~Provider() { BCryptCloseAlgorithmProvider(handle, 0); }
};
BCRYPT_ALG_HANDLE provider() {
  static Provider instance;
  return instance.handle;
}
} // namespace
struct Sha256::State {
  BCRYPT_HASH_HANDLE hash = nullptr;
  ~State() { if (hash) BCryptDestroyHash(hash); }
};
Sha256::Sha256() : state_(std::make_unique<State>()) {
  // Share the immutable provider; each digest owns its mutable hash state.
  if (BCryptCreateHash(provider(), &state_->hash, nullptr, 0,
                       nullptr, 0, 0) < 0)
    throw std::runtime_error("cannot create SHA-256 hash");
}
Sha256::~Sha256() = default;
void Sha256::update(std::span<const std::uint8_t> bytes) {
  if (!state_) throw std::logic_error("SHA-256 already finished");
  while (!bytes.empty()) {
    const auto count = static_cast<ULONG>(std::min<std::size_t>(bytes.size(), std::numeric_limits<ULONG>::max()));
    if (BCryptHashData(state_->hash, const_cast<PUCHAR>(bytes.data()), count, 0) < 0)
      throw std::runtime_error("cannot update SHA-256 hash");
    bytes = bytes.subspan(count);
  }
}
std::string Sha256::finish() {
  if (!state_) throw std::logic_error("SHA-256 already finished");
  std::array<unsigned char, 32> bytes;
  if (BCryptFinishHash(state_->hash, bytes.data(), bytes.size(), 0) < 0)
    throw std::runtime_error("cannot finish SHA-256 hash");
  state_.reset();
  std::string hex;
  hex.reserve(64);
  for (auto byte : bytes) {
    hex += "0123456789abcdef"[byte >> 4];
    hex += "0123456789abcdef"[byte & 15];
  }
  return hex;
}
} // namespace venture::digest
