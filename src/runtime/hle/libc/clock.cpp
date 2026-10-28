#include "vfs.h"
#include "abi32.h"
#include "platform/memory.h"
#include "core/profile.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace venture::hle::libc {
void register_timers(HleLibrary&);
std::int64_t realtime_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
int deadline(const Timespec* t, int clock, std::int64_t* out) {
  if (!t || t->nsec < 0 || t->nsec >= 1000000000 || (clock != 0 && clock != 1)) return -err::einval;
  if (t->sec > (INT64_MAX - t->nsec) / 1000000000) {
    *out = INT64_MAX;
    return 0;
  }
  if (t->sec < 0) {
    *out = 0;
    return 0;
  }
  auto n = t->sec * 1000000000 + t->nsec;
  if (clock == 0) {
    auto delta = n - realtime_ns();
    auto now = monotonic_ns();
    n = delta > INT64_MAX - now ? INT64_MAX : std::max<std::int64_t>(0, now + delta);
  }
  *out = n;
  return 0;
}
namespace {
std::int64_t clock_ns(int id) {
  switch (id) {
    case 0:
    case 5:
      return realtime_ns();
    case 1:
    case 4:
    case 6:
    case 7:
      return monotonic_ns();
    case 2:
      return host::cpu_time(false);
    case 3:
      return host::cpu_time(true);
    default:
      return -err::einval;
  }
}
int gettime(int id, Timespec* t) {
  auto n = clock_ns(id);
  if (n < 0) return result(n);
  if (!t) return failure(err::efault);
  const Timespec value{n / 1000000000, n % 1000000000};
  if (!platform::guarded_store(h2g(t), &value, sizeof(value))) return failure(err::efault);
  return 0;
}
int getres(int id, Timespec* t) {
  if (clock_ns(id) < 0) return failure(err::einval);
  const Timespec value{0, (id == 5 || id == 6) ? 1000000 : 100};
  if (t && !platform::guarded_store(h2g(t), &value, sizeof(value))) return failure(err::efault);
  return 0;
}
int gettimeofday_fn(Timeval* t, void* zone) {
  auto n = realtime_ns();
  if (t) *t = {n / 1000000000, (n / 1000) % 1000000};
  if (zone) std::memset(zone, 0, 8);
  return 0;
}
std::int64_t time_fn(std::int64_t* out) {
  auto t = realtime_ns() / 1000000000;
  if (out) *out = t;
  return t;
}
int clock_sleep(int id, int flags, const Timespec* t, Timespec* remain) {
  if (!t || t->sec < 0 || t->nsec < 0 || t->nsec >= 1000000000 || (flags & ~1)) return err::einval;
  if (id != 0 && id != 1 && id != 7) return err::einval;
  std::int64_t end;
  int r = deadline(t, id == 7 ? 1 : id, &end);
  if (r) return -r;
  if (!(flags & 1)) {
    auto now = monotonic_ns();
    if (t->sec > (INT64_MAX - now - t->nsec) / 1000000000)
      end = INT64_MAX;
    else
      end = now + t->sec * 1000000000 + t->nsec;
  }
  std::int64_t requested = -1;
  if (profile::futex_timing_enabled) {
    if (flags & 1) {
      requested = end - monotonic_ns();
      if (requested <= 0) requested = -4; // Expired absolute sleep is distinct from a relative zero.
    }
    else if (t->sec > INT64_MAX / 1000000000 || t->sec * 1000000000 > INT64_MAX - t->nsec)
      requested = INT64_MAX;
    else requested = t->sec * 1000000000 + t->nsec;
  }
  r = sleep_until(*GuestThread::current(), end, requested);
  if (r && remain && !(flags & 1)) {
    auto left = std::max<std::int64_t>(0, end - monotonic_ns());
    *remain = {left / 1000000000, left % 1000000000};
  }
  return -r;
}
int nanosleep_fn(const Timespec* t, Timespec* left) {
  int r = clock_sleep(1, 0, t, left);
  return r ? failure(r) : 0;
}
int usleep_fn(std::uint32_t micros) {
  Timespec t{micros / 1000000, (micros % 1000000) * 1000};
  return nanosleep_fn(&t, nullptr);
}
unsigned sleep_fn(unsigned seconds) {
  Timespec t{seconds, 0}, left{};
  if (nanosleep_fn(&t, &left)) return static_cast<unsigned>(left.sec + (left.nsec != 0));
  return 0;
}
std::int64_t clock_fn() {
  auto n = host::cpu_time(false);
  return n < 0 ? -1 : n / 1000;
}  // bionic CLOCKS_PER_SEC=1,000,000.
std::int64_t times_fn(std::int64_t* t) {
  auto n = host::cpu_time(false);
  if (t) {
    t[0] = n / 10000000;
    t[1] = t[2] = t[3] = 0;
  }
  return monotonic_ns() / 10000000;
}
struct Itimerspec {
  Timespec interval, value;
};
class TimerFd final : public File {
 public:
  bool readiness_notifications() const override { return true; }
  bool epoll_supported() const override { return true; }
  explicit TimerFd(int clock) : clock_(clock) { flags = 0; }
  int set(int options, const Itimerspec* t, Itimerspec* old) {
    if (options & ~1 || !t || t->interval.sec < 0 || t->value.sec < 0 || t->interval.nsec < 0 ||
        t->interval.nsec >= 1000000000 || t->value.nsec < 0 || t->value.nsec >= 1000000000)
      return -err::einval;
    if (t->interval.sec > (INT64_MAX - t->interval.nsec) / 1000000000 ||
        t->value.sec > (INT64_MAX - t->value.nsec) / 1000000000)
      return -err::eoverflow;
    auto period = t->interval.sec * 1000000000 + t->interval.nsec;
    auto next = t->value.sec * 1000000000 + t->value.nsec;
    if (next && !(options & 1)) {
      auto now = now_ns();
      if (next > INT64_MAX - now) return -err::eoverflow;
      next += now;
    }
    std::lock_guard lock(mutex);
    if (old) get_locked(old);
    period_ = period;
    next_ = next;
    ++read_epoch;
    readiness.notify(poll_in);
    return 0;
  }
  int get(Itimerspec* t) {
    std::lock_guard lock(mutex);
    get_locked(t);
    return 0;
  }
  short ready(short e) override {
    std::lock_guard lock(mutex);
    return next_ && now_ns() >= next_ ? e & poll_in : 0;
  }
  std::int64_t readiness_time(short events) override {
    std::lock_guard lock(mutex);
    if (!(events & poll_in) || !next_) return -1;
    // Convert a realtime timer's deadline at observation, preserving its
    // realtime semantics. This clock-pair has diagnostic sampling uncertainty.
    return clock_ == 0 ? monotonic_ns() + (next_ - realtime_ns()) : next_;
  }
  std::int64_t readiness_deadline(short events) override {
    std::lock_guard lock(mutex);
    if (!(events & poll_in) || !next_) return -1;
    const auto now = now_ns();
    // An unread expiry is level ready, not a past deadline to spin against
    // when an EPOLLET waiter has already consumed that readiness edge.
    if (now >= next_) return -1;
    return clock_ == 0 ? monotonic_ns() + (next_ - now) : next_;
  }
  std::int64_t read(void* p, std::uint64_t n) override {
    if (n < 8) return -err::einval;
    sync_census::IoScope trace(sync_census::IoSource::timerfd_read,
        sync_census::enabled && GuestThread::current() ? GuestThread::current()->tid() : 0);
    for (;;) {
      {
        std::lock_guard lock(mutex);
        auto now = now_ns();
        if (next_ && now >= next_) {
          if (sync_census::enabled) trace.readiness(clock_ == 0 ? monotonic_ns() + (next_ - now) : next_);
          std::uint64_t count = period_ ? 1 + (now - next_) / period_ : 1;
          ++read_epoch;
          next_ = period_ ? next_ + count * period_ : 0;
          readiness.notify(poll_in);
          std::memcpy(p, &count, 8);
          return 8;
        }
      }
      if (flags.load() & o_nonblock) return -err::eagain;
      const auto sleep_begin = sync_census::enabled ? monotonic_ns() : 0;
      int r = wait_ready(poll_in, INT64_MAX);
      if (sync_census::enabled) trace.slept(sleep_begin, monotonic_ns());
      if (r) return r;
    }
  }

 private:
  std::int64_t now_ns() const { return clock_ == 0 ? realtime_ns() : monotonic_ns(); }
  void get_locked(Itimerspec* t) {
    auto now = now_ns();
    auto left = next_ ? std::max<std::int64_t>(0, next_ - now) : 0;
    // Report the next periodic deadline without consuming unread expirations.
    if (next_ && period_ && now >= next_) left = period_ - (now - next_) % period_;
    *t = {{period_ / 1000000000, period_ % 1000000000}, {left / 1000000000, left % 1000000000}};
  }
  int clock_;
  std::int64_t next_ = 0, period_ = 0;
};
int timerfd_create_fn(int id, int flags) {
  if ((id != 0 && id != 1 && id != 7) || (flags & ~(o_cloexec | o_nonblock)))
    return failure(err::einval);
  auto f = std::make_shared<TimerFd>(id);
  f->flags = flags & ~o_cloexec;
  return result(FdTable::get().insert(f, 0, flags & o_cloexec));
}
int timerfd_set_fn(int fd, int flags, const Itimerspec* t, Itimerspec* old) {
  auto f = FdTable::get().file(fd);
  if (!f) return failure(err::ebadf);
  auto timer = std::dynamic_pointer_cast<TimerFd>(f);
  return timer ? result(timer->set(flags, t, old)) : failure(err::einval);
}
int timerfd_get_fn(int fd, Itimerspec* t) {
  auto f = FdTable::get().file(fd);
  if (!f) return failure(err::ebadf);
  if (!t) return failure(err::efault);
  auto timer = std::dynamic_pointer_cast<TimerFd>(f);
  return timer ? result(timer->get(t)) : failure(err::einval);
}
int gettime32(int id, abi32::Timespec* t) {
  const auto n = clock_ns(id);
  if (n < 0) return result(n);
  if (!t) return failure(err::efault);
  if (n / 1000000000 > INT32_MAX) return failure(err::eoverflow);
  const abi32::Timespec value{std::int32_t(n / 1000000000), std::int32_t(n % 1000000000)};
  if (!platform::guarded_store(h2g(t), &value, sizeof(value))) return failure(err::efault);
  return 0;
}
// bionic ARM clock_t/time_t are signed 32-bit longs. These interfaces
// return the low word, including negative wrapped values, without EOVERFLOW.
std::int32_t clock32() {
  return static_cast<std::int32_t>(static_cast<std::uint32_t>(clock_fn()));
}
std::int32_t times32(std::int32_t* t) {
  std::int64_t wide[4];
  auto ticks = times_fn(t ? wide : nullptr);
  if (t) for (int i = 0; i < 4; ++i) t[i] = static_cast<std::int32_t>(static_cast<std::uint32_t>(wide[i]));
  return static_cast<std::int32_t>(static_cast<std::uint32_t>(ticks));
}
std::int32_t time32(std::int32_t* out) {
  auto seconds = static_cast<std::int32_t>(static_cast<std::uint32_t>(time_fn(nullptr)));
  if (out) *out = seconds;
  return seconds;
}
int getres32(int id, abi32::Timespec* t) {
  if (clock_ns(id) < 0) return failure(err::einval);
  const abi32::Timespec value{0, (id == 5 || id == 6) ? 1000000 : 100};
  if (t && !platform::guarded_store(h2g(t), &value, sizeof(value))) return failure(err::efault);
  return 0;
}
int gettimeofday32(abi32::Timeval* t, void* zone) {
  Timeval wide; auto r = gettimeofday_fn(t ? &wide : nullptr, zone);
  if (!r && t) {
    if (wide.sec > INT32_MAX) return failure(err::eoverflow);
    *t = {std::int32_t(wide.sec), std::int32_t(wide.usec)};
  }
  return r;
}
int clock_sleep32(int id, int flags, const abi32::Timespec* t, abi32::Timespec* remain) {
  if (!t) return err::efault;
  Timespec wide{t->sec, t->nsec}, left{};
  auto r = clock_sleep(id, flags, &wide, remain ? &left : nullptr);
  if (r == err::eintr && remain && !(flags & 1)) *remain = {std::int32_t(left.sec), std::int32_t(left.nsec)};
  return r;
}
int nanosleep32(const abi32::Timespec* t, abi32::Timespec* left) {
  auto r = clock_sleep32(1, 0, t, left); return r ? failure(r) : 0;
}
struct Itimerspec32 { abi32::Timespec interval, value; };
int timerfd_set32(int fd, int flags, const Itimerspec32* t, Itimerspec32* old) {
  if (!t) return failure(err::efault);
  Itimerspec wide{{t->interval.sec, t->interval.nsec}, {t->value.sec, t->value.nsec}}, previous{};
  auto r = timerfd_set_fn(fd, flags, &wide, old ? &previous : nullptr);
  if (!r && old) *old = {{std::int32_t(previous.interval.sec), std::int32_t(previous.interval.nsec)}, {std::int32_t(previous.value.sec), std::int32_t(previous.value.nsec)}};
  return r;
}
int timerfd_get32(int fd, Itimerspec32* t) {
  if (!t) return failure(err::efault);
  Itimerspec wide; auto r = timerfd_get_fn(fd, &wide);
  if (!r) *t = {{std::int32_t(wide.interval.sec), std::int32_t(wide.interval.nsec)}, {std::int32_t(wide.value.sec), std::int32_t(wide.value.nsec)}};
  return r;
}
}  // namespace
void register_clock(HleLibrary& lib) {
  if (process_config().isa() == GuestIsa::a32) {
    abi32::adapt<&clock32>("clock"); abi32::adapt<&times32>("times"); abi32::adapt<&time32>("time");
    abi32::adapt<&gettime32>("clock_gettime"); abi32::adapt<&getres32>("clock_getres");
    abi32::adapt<&gettimeofday32>("gettimeofday"); abi32::adapt<&clock_sleep32>("clock_nanosleep");
    abi32::adapt<&nanosleep32>("nanosleep");
    abi32::adapt<&timerfd_get32>("timerfd_gettime"); abi32::adapt<&timerfd_set32>("timerfd_settime");
  }
  register_timers(lib);
#define B(n, f) VENTURE_LIBC_BIND(lib, n, f)
  // Queries cannot block a guest wait, deliver signals or re-enter guest code.
  // Integer-only callbacks retain host FP state; output faults return EFAULT.
  add(lib, "clock_gettime", wrap<&gettime>(), Execution::InJit);
  add(lib, "clock_getres", wrap<&getres>(), Execution::InJit);
  B("clock_nanosleep", clock_sleep);
  B("gettimeofday", gettimeofday_fn);
  B("time", time_fn);
  B("nanosleep", nanosleep_fn);
  B("usleep", usleep_fn);
  B("sleep", sleep_fn);
  B("clock", clock_fn);
  B("times", times_fn);
  B("timerfd_create", timerfd_create_fn);
  B("timerfd_settime", timerfd_set_fn);
  B("timerfd_gettime", timerfd_get_fn);
#undef B
}
}  // namespace venture::hle::libc
