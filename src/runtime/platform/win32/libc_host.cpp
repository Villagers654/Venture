#include "platform/libc_host.h"

// bcrypt.h requires the Win32 type declarations first.
// clang-format off
#include <windows.h>
#include <bcrypt.h>
// clang-format on
#include <io.h>

#include <algorithm>
#include <cerrno>

#include "hle/libc/linux_errors.h"
namespace venture::platform {
void close_host_fd(int fd) { _close(fd); }
int libc_host_error() {
  using namespace hle::err;
  switch (errno) {
    case EINVAL:
      return kEINVAL;
    case ENOMEM:
      return kENOMEM;
    case ENOENT:
      return kENOENT;
    case EACCES:
      return kEACCES;
    case EBADF:
      return kEBADF;
    case ENOSPC:
      return kENOSPC;
    case EFBIG:
      return kEFBIG;
    case EPIPE:
      return kEPIPE;
    default:
      return kEIO;
  }
}
bool secure_random(void* out, std::uint64_t size) {
  auto p = static_cast<unsigned char*>(out);
  while (size) {
    auto n = static_cast<ULONG>(std::min<std::uint64_t>(size, ULONG_MAX));
    if (BCryptGenRandom(nullptr, p, n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return false;
    p += n;
    size -= n;
  }
  return true;
}
bool local_time(std::int64_t seconds, std::tm& result) { return _localtime64_s(&result, &seconds) == 0; }
bool make_local_time(std::tm& value, std::int64_t& seconds) {
  errno = 0;
  seconds = _mktime64(&value);
  return seconds != -1 || errno == 0;
}
TimeZone time_zone() {
  _tzset();
  long west = 0;
  int dst = 0;
  _get_timezone(&west);
  _get_daylight(&dst);
  char standard[128]{}, summer[128]{};
  std::size_t n;
  _get_tzname(&n, standard, sizeof(standard), 0);
  _get_tzname(&n, summer, sizeof(summer), 1);
  return {west, dst != 0, standard, summer};
}
}  // namespace venture::platform
