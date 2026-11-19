// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/sha256_portable.h"

#include <algorithm>
#include <cstring>

namespace venture::digest::portable {
namespace {
constexpr std::uint32_t kRound[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
constexpr std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
}  // namespace

void Sha256::block(const std::uint8_t* data) {
  std::uint32_t w[64];
  for (int i = 0; i < 16; ++i)
    w[i] = std::uint32_t(data[4 * i]) << 24 | std::uint32_t(data[4 * i + 1]) << 16 |
           std::uint32_t(data[4 * i + 2]) << 8 | std::uint32_t(data[4 * i + 3]);
  for (int i = 16; i < 64; ++i) {
    const auto s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const auto s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  auto [a, b, c, d, e, f, g, h] = state_;
  for (int i = 0; i < 64; ++i) {
    const auto t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kRound[i] + w[i];
    const auto t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
  state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
}

void Sha256::update(std::span<const std::uint8_t> bytes) {
  length_ += bytes.size();
  auto* data = bytes.data();
  auto size = bytes.size();
  if (buffered_) {
    const auto take = std::min(size, buffer_.size() - buffered_);
    std::memcpy(buffer_.data() + buffered_, data, take);
    buffered_ += take; data += take; size -= take;
    if (buffered_ < buffer_.size()) return;
    block(buffer_.data());
    buffered_ = 0;
  }
  for (; size >= 64; data += 64, size -= 64) block(data);
  if (size) std::memcpy(buffer_.data(), data, size);
  buffered_ = size;
}

std::array<std::uint8_t, 32> Sha256::finish() {
  const auto bits = length_ * 8;
  buffer_[buffered_++] = 0x80;
  if (buffered_ > 56) {
    std::memset(buffer_.data() + buffered_, 0, 64 - buffered_);
    block(buffer_.data());
    buffered_ = 0;
  }
  std::memset(buffer_.data() + buffered_, 0, 56 - buffered_);
  for (int i = 0; i < 8; ++i) buffer_[56 + i] = std::uint8_t(bits >> (56 - 8 * i));
  block(buffer_.data());
  std::array<std::uint8_t, 32> out;
  for (int i = 0; i < 8; ++i)
    for (int j = 0; j < 4; ++j) out[4 * i + j] = std::uint8_t(state_[i] >> (24 - 8 * j));
  return out;
}
}  // namespace venture::digest::portable
