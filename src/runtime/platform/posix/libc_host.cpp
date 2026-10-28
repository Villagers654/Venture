#include "platform/libc_host.h"

#include <sys/random.h>
#include <unistd.h>

#include <cerrno>
#include <limits>
namespace venture::platform {
void close_host_fd(int fd) { ::close(fd); }
int libc_host_error() { return errno; }  // Linux host uses the guest's asm-generic values.
bool secure_random(void* out, std::uint64_t size) {
  auto p = static_cast<unsigned char*>(out);
  while (size) {
    auto n = ::getrandom(p, size, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (!n) return false;
    p += n;
    size -= n;
  }
  return true;
}
bool local_time(std::int64_t seconds, std::tm& result) {
  time_t t = seconds;
  return localtime_r(&t, &result) != nullptr;
}
bool make_local_time(std::tm& value, std::int64_t& seconds) {
  errno = 0;
  seconds = ::mktime(&value);
  return seconds != -1 || errno == 0;
}
TimeZone time_zone() {
  ::tzset();
  return {::timezone, ::daylight != 0, ::tzname[0], ::tzname[1]};
}
}  // namespace venture::platform
