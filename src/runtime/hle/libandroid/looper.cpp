#include "android/errors.h"
#include "hle/libandroid/parts.h"
#include "android/looper.h"
#include "core/wait.h"
#include <algorithm>
#include <climits>
namespace venture::hle::android_hle {
namespace {
using namespace venture::android;
int add_fd(Looper* p, int fd, int ident, int events, GuestAddr cb, GuestAddr data) {
  auto l = pin_looper(p);
  return l ? l->add_fd(fd, ident, events, cb, data) : -1;
}
int remove_fd(Looper* p, int fd) {
  auto l = pin_looper(p);
  return l ? l->remove_fd(fd) : -1;
}
void wake(Looper* p) {
  if (auto l = pin_looper(p)) l->wake();
}
int poll_once(int timeout, int* fd, int* events, GuestAddr* data) {
  auto l = pin_looper(thread_looper());
  return l ? l->poll(timeout, fd, events, data) : -4;
}
int poll_all(int timeout, int* fd, int* events, GuestAddr* data) {
  auto end = timeout < 0 ? INT64_MAX : monotonic_ns() + std::int64_t(timeout) * 1000000;
  for (;;) {
    int result = poll_once(timeout, fd, events, data);
    if (result != -2) return result;
    if (end != INT64_MAX) {
      auto left = end - monotonic_ns();
      if (left <= 0) return -3;
      timeout = static_cast<int>(std::min<std::int64_t>(INT_MAX, (left + 999999) / 1000000));
    }
  }
}
}  // namespace
void register_looper(HleLibrary& lib) {
  add<&android::prepare_looper>(lib, "ALooper_prepare");
  add<&android::thread_looper>(lib, "ALooper_forThread");
  add<&android::acquire_looper>(lib, "ALooper_acquire");
  add<&android::release_looper>(lib, "ALooper_release");
  add<&add_fd>(lib, "ALooper_addFd");
  add<&remove_fd>(lib, "ALooper_removeFd");
  add<&wake>(lib, "ALooper_wake");
  add<&poll_once>(lib, "ALooper_pollOnce");
  add<&poll_all>(lib, "ALooper_pollAll");
}
}  // namespace venture::hle::android_hle
