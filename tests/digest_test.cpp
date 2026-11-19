// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/digest.h"
#include "common/sha256_portable.h"
#include <doctest/doctest.h>
#include <algorithm>
#include <array>
#include <sstream>
#include <vector>
using namespace venture;
TEST_CASE("SHA-256 binary vectors and streaming") {
  const std::array<std::pair<std::size_t, const char*>, 5> vectors{{
      {0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
      {55, "463eb28e72f82e0a96c0a4cc53690c571281131f672aa229e0d45ae59b598b59"},
      {56, "da2ae4d6b36748f2a318f23e7ab1dfdf45acdc9d049bd80e59de82a60895f562"},
      {64, "fdeab9acf3710362bd2658cdc9a29e8f9c757fcf9811603a8c447cd1d9151108"},
      {3145745, "69112280f593fd44684d97d3fe42cdcb84da4ae495d8e1db649f06c3596a7ce6"},
  }};
  for (const auto& [size, expected] : vectors) {
    CAPTURE(size);
    std::vector<std::uint8_t> bytes(size);
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::uint8_t>(i);
    CHECK(digest::sha256(bytes) == expected);
    digest::Sha256 incremental;
    for (std::size_t i = 0; i < bytes.size(); i += 17)
      incremental.update(std::span(bytes).subspan(i, std::min<std::size_t>(17, bytes.size() - i)));
    CHECK(incremental.finish() == expected);
    std::string text(bytes.begin(), bytes.end());
    std::istringstream input(text);
    CHECK(digest::sha256(input) == expected);
    CHECK(input.eof());
  }
  digest::Sha256 finalized;
  finalized.finish();
  CHECK_THROWS_AS(finalized.finish(), std::logic_error);
  CHECK_THROWS_AS(finalized.update({}), std::logic_error);
  std::istringstream failed("input");
  failed.setstate(std::ios::badbit);
  CHECK_THROWS_AS(digest::sha256(failed), std::runtime_error);
}
TEST_CASE("the portable SHA-256 (Android) matches the system provider") {
  // Every length across the one- and two-block padding edges, fed whole and in
  // uneven pieces, plus a multi-megabyte input.
  const auto hex = [](const std::array<std::uint8_t, 32>& bytes) {
    std::string out;
    for (auto byte : bytes) {
      out += "0123456789abcdef"[byte >> 4];
      out += "0123456789abcdef"[byte & 15];
    }
    return out;
  };
  std::vector<std::uint8_t> bytes(3145745);
  for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::uint8_t>(i * 7 + (i >> 9));
  for (std::size_t size = 0; size <= 300; ++size) {
    CAPTURE(size);
    const auto input = std::span<const std::uint8_t>(bytes).first(size);
    digest::portable::Sha256 whole;
    whole.update(input);
    const auto expected = digest::sha256(input);
    CHECK(hex(whole.finish()) == expected);
    digest::portable::Sha256 pieces;
    for (std::size_t at = 0, step = 1; at < size; at += step, step = step % 67 + 13)
      pieces.update(input.subspan(at, std::min(step, size - at)));
    CHECK(hex(pieces.finish()) == expected);
  }
  digest::portable::Sha256 large;
  large.update(bytes);
  CHECK(hex(large.finish()) == digest::sha256(bytes));
}
