// Include before host standard headers (which define errno macros).
#pragma once
#include "core/linux_errno.h"
namespace venture::android::err {
inline constexpr int invalid = linux_errno::EINVAL;
inline constexpr int io = linux_errno::EIO;
inline constexpr int no_memory = linux_errno::ENOMEM;
inline constexpr int bad_fd = linux_errno::EBADF;
inline constexpr int fault = linux_errno::EFAULT;
inline constexpr int overflow = linux_errno::EOVERFLOW;
inline constexpr int unsupported = linux_errno::ENOSYS;
inline constexpr int no_device = linux_errno::ENODEV;
inline constexpr int again = linux_errno::EAGAIN;
inline constexpr int busy = linux_errno::EBUSY;
inline constexpr int interrupted = linux_errno::EINTR;
inline constexpr int timed_out = linux_errno::ETIMEDOUT;
inline constexpr int not_supported = linux_errno::EOPNOTSUPP;
}  // namespace venture::android::err
