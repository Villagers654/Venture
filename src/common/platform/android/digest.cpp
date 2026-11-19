// SPDX-License-Identifier: GPL-3.0-or-later
// The NDK offers native code no stable crypto API: the portable SHA-256.
#include "common/digest.h"
#include "common/sha256_portable.h"

#include <stdexcept>

namespace venture::digest {
struct Sha256::State {
  portable::Sha256 hash;
};
Sha256::Sha256() : state_(std::make_unique<State>()) {}
Sha256::~Sha256() = default;
void Sha256::update(std::span<const std::uint8_t> bytes) {
  if (!state_) throw std::logic_error("SHA-256 already finished");
  state_->hash.update(bytes);
}
std::string Sha256::finish() {
  if (!state_) throw std::logic_error("SHA-256 already finished");
  const auto bytes = state_->hash.finish();
  state_.reset();
  std::string hex;
  hex.reserve(64);
  for (auto byte : bytes) {
    hex += "0123456789abcdef"[byte >> 4];
    hex += "0123456789abcdef"[byte & 15];
  }
  return hex;
}
}  // namespace venture::digest
