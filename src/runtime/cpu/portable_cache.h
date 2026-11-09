#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace venture {
// Persistent cache of compiled A64 blocks (Dynarmic portable_block.h): one
// append-only file per game and runtime build. A block found here installs
// instead of being translated. Never contains guest data; every block is
// checked against the guest code it was translated from before it installs.
class PortableBlockCache {
 public:
  // Null when disabled (VENTURE_JIT_PTC=0; on by default) or without a cache directory.
  static PortableBlockCache* get();
  void store(std::uint64_t descriptor, const std::uint8_t* data, std::size_t size) noexcept;
  // The bytes are decoded into the caller's `buffer` and stay valid until it changes. Callers own
  // it: guest code also runs during host-thread exit (an adopted thread's TLS destructors), after
  // the thread_local objects constructed later than its guest thread are already destroyed.
  std::pair<const std::uint8_t*, std::size_t> find(std::uint64_t descriptor, std::vector<std::uint8_t>& buffer) noexcept;
  // Writes every stored block (tests, exit).
  void flush() noexcept;
  // Background translation (cpu/pretranslate.cpp): whether stores are kept,
  // whether a block is present, and a copy of its record without counting a use.
  bool accepting() const noexcept;
  bool contains(std::uint64_t descriptor) noexcept;
  bool copy(std::uint64_t descriptor, std::vector<std::uint8_t>& out) noexcept;
  // Whether an earlier run's background translation explored this block's successors
  // (the file's last explored mark covers its record), so this run need not read it again.
  bool explored(std::uint64_t descriptor) const noexcept;
  // Background translation has nothing queued: marks every record written so far explored,
  // bar those `explored` rejects (called with the writer's lock held). False if none was new.
  bool mark_explored(const std::function<bool(std::uint64_t)>& explored) noexcept;
  // Stored records the writer has not written yet hold at most this much memory: past it a
  // store is dropped (the block is translated again when next needed). Background
  // translation waits below half of it, so drops are left to guest threads' bursts.
  static constexpr std::uint64_t kUnwrittenLimit = 128ull << 20;
  std::uint64_t unwritten_bytes() const noexcept;
  struct Impl;
 private:
  explicit PortableBlockCache(Impl* impl) : impl_(impl) {}
  Impl* impl_;
};
} // namespace venture
