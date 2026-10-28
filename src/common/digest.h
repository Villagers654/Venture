// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <istream>
#include <memory>
#include <span>
#include <string>

namespace venture::digest {
// Provider failures throw; finish consumes the state. Binary input is unmodified.
class Sha256 {
 public:
  Sha256();
  ~Sha256();
  Sha256(const Sha256&) = delete;
  Sha256& operator=(const Sha256&) = delete;
  void update(std::span<const std::uint8_t> bytes);
  std::string finish();
 private:
  struct State;
  std::unique_ptr<State> state_;
};
std::string sha256(std::span<const std::uint8_t> bytes);
// Reads bounded chunks; throws on a read failure, leaving EOF visible to callers.
std::string sha256(std::istream& input);
} // namespace venture::digest
