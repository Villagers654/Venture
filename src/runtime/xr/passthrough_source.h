#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace venture::xr {
// An external passthrough source: a local program that shows the real world
// under the application's alpha where the host runtime cannot. TEMPORARY: the
// only one is the Steam Link native passthrough companion, spoken to over its
// own SBN1 protocol (xr/passthrough_source_sbn1.h, Windows). This backend goes
// with that transport once the companion offers a standard OpenXR path, which
// the native and blend backends already use.
//
// One show-the-world lease per process. All endpoint I/O runs on its worker;
// the render seam reads only the current atomic generation.
class PassthroughSource {
 public:
  PassthroughSource();
  ~PassthroughSource();
  void request(bool active);
  std::uint64_t permission() const;
  // True only after the worker closed its connection for the current withdrawal.
  bool withdrawn() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
// A source answered a probe within `wait_ms`.
bool passthrough_source_answers(unsigned wait_ms);

namespace passthrough_source_platform {
// One connection to a source. transfer() moves exactly `size` bytes, or
// fails when the peer closes, `alive` turns false (checked at least every
// 100 ms) or the steady-clock millisecond `deadline` (0: none) passes.
struct Channel {
  virtual ~Channel() = default;
  virtual bool transfer(bool writing, void* data, std::size_t size,
                        const std::function<bool()>& alive, std::uint64_t deadline) = 0;
};
// Once some but not all bytes of a transfer have moved, a two-second deadline applies.
std::uint32_t process_id();
bool random(void* data, std::size_t size);
std::uint64_t now_ms();
} // namespace passthrough_source_platform
} // namespace venture::xr
