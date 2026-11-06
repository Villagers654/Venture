// The external passthrough source's POSIX plumbing: process ID, randomness and
// time. Its only transport (SBN1) exists on Windows only.
#include "xr/passthrough_source.h"
#include <cerrno>
#include <ctime>
#include <sys/random.h>
#include <unistd.h>

namespace venture::xr::passthrough_source_platform {
std::uint32_t process_id() { return std::uint32_t(getpid()); }
bool random(void* data, std::size_t size) {
  auto* bytes = static_cast<std::uint8_t*>(data);
  for (std::size_t used = 0; used < size;) {
    const auto count = getrandom(bytes + used, size - used, 0);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    used += std::size_t(count);
  }
  return true;
}
std::uint64_t now_ms() {
  timespec value{};
  clock_gettime(CLOCK_MONOTONIC, &value);
  return std::uint64_t(value.tv_sec) * 1000 + std::uint64_t(value.tv_nsec) / 1000000;
}
} // namespace venture::xr::passthrough_source_platform
