// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/digest.h"
#include <array>
#include <stdexcept>
#include <openssl/evp.h>

namespace venture::digest {
struct Sha256::State {
  EVP_MD_CTX* hash = EVP_MD_CTX_new();
  ~State() { EVP_MD_CTX_free(hash); }
};
Sha256::Sha256() : state_(std::make_unique<State>()) {
  if (!state_->hash || EVP_DigestInit_ex(state_->hash, EVP_sha256(), nullptr) != 1)
    throw std::runtime_error("cannot create SHA-256 hash");
}
Sha256::~Sha256() = default;
void Sha256::update(std::span<const std::uint8_t> bytes) {
  if (!state_) throw std::logic_error("SHA-256 already finished");
  if (!bytes.empty() && EVP_DigestUpdate(state_->hash, bytes.data(), bytes.size()) != 1)
    throw std::runtime_error("cannot update SHA-256 hash");
}
std::string Sha256::finish() {
  if (!state_) throw std::logic_error("SHA-256 already finished");
  std::array<unsigned char, 32> bytes;
  unsigned size = 0;
  if (EVP_DigestFinal_ex(state_->hash, bytes.data(), &size) != 1 || size != bytes.size())
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
