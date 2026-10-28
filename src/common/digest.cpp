// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/digest.h"
#include <array>
#include <stdexcept>

namespace venture::digest {
std::string sha256(std::span<const std::uint8_t> bytes) {
  Sha256 hash;
  hash.update(bytes);
  return hash.finish();
}
std::string sha256(std::istream& input) {
  Sha256 hash;
  std::array<std::uint8_t, 65536> bytes;
  while (input.read(reinterpret_cast<char*>(bytes.data()), bytes.size()) || input.gcount())
    hash.update(std::span(bytes).first(static_cast<std::size_t>(input.gcount())));
  if (!input.eof() || input.bad()) throw std::runtime_error("cannot read SHA-256 input");
  return hash.finish();
}
} // namespace venture::digest
