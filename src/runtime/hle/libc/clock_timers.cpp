#include "vfs.h"
#include "core/signals.h"
#include "core/thread_api.h"
#include "common/log.h"
#include <algorithm>
#include <climits>
#include <condition_variable>
#include <thread>
namespace venture::hle::libc {
namespace {
struct Itimer {
  Timespec interval, value;
};
struct Sigevent {
  std::uint64_t value;
  int signo, notify;
  GuestAddr function, attr;
  char reserved[32];
};
static_assert(sizeof(Sigevent) == 64);
struct Timer {
  int clock = 1;
  Sigevent event{};
  std::int64_t next = 0, period = 0;
  int overrun = 0;
};
class Timers {
 public:
  std::mutex mutex;
  std::condition_variable cv;
  std::map<GuestAddr, Timer> timers;
  Timers() : worker_([this](std::stop_token stop) { run(stop); }) {}
  ~Timers() {
    worker_.request_stop();
    cv.notify_all();
  }

 private:
  void run(std::stop_token stop) {
    std::unique_lock lock(mutex);
    while (!stop.stop_requested()) {
      struct Due { Sigevent event; std::int64_t deadline; };
      std::vector<Due> due;
      std::int64_t delay = 1000000000;
      for (auto& [id, t] : timers) {
        if (!t.next) continue;
        auto now = t.clock == 0 ? realtime_ns() : monotonic_ns();
        if (now >= t.next) {
          const auto deadline = sync_census::enabled
              ? (t.clock == 0 ? monotonic_ns() + (t.next - now) : t.next) : -1;
          auto count = t.period ? 1 + (now - t.next) / t.period : 1;
          t.overrun = static_cast<int>(std::min<std::int64_t>(count - 1, INT_MAX));
          t.next = t.period ? t.next + count * t.period : 0;
          due.push_back({t.event, deadline});
        }
        if (t.next) delay = std::min(delay, std::max<std::int64_t>(1, t.next - now));
      }
      lock.unlock();
      for (auto& entry : due) {
        auto& e = entry.event;
        sync_census::IoScope trace(sync_census::IoSource::timer_dispatch, 0);
        if (sync_census::enabled) trace.readiness(entry.deadline);
        if (e.notify == 0)
          signals::send_to_process({.signo = e.signo, .code = -2, .value = e.value});
        else if (e.notify == 2) {
          GuestAddr handle;
          threads::Options options;
          options.detached = true;
          options.on_exit = run_key_destructors;
          int r = threads::create(options, e.function, e.value, &handle);
          if (r)
            log::warn("timer", "guest timer callback thread creation failed with Linux errno {}",
                      -r);
        }
      }
      lock.lock();
      if (!stop.stop_requested()) {
        const auto begin = sync_census::enabled ? monotonic_ns() : 0;
        sync_census::IoScope trace(sync_census::IoSource::timer_service, 0, begin + delay);
        auto status = cv.wait_for(lock, std::chrono::nanoseconds(delay));
        if (sync_census::enabled) trace.slept(begin, monotonic_ns());
        if (status == std::cv_status::timeout) trace.timeout();
      }
    }
  }
  // Declared last so it joins before maps and synchronization objects die.
  std::jthread worker_;
};
Timers& service() {
  static Timers t;
  return t;
}
void get_locked(const Timer& t, Itimer* out) {
  auto now = t.clock == 0 ? realtime_ns() : monotonic_ns();
  auto left = t.next ? std::max<std::int64_t>(0, t.next - now) : 0;
  *out = {{t.period / 1000000000, t.period % 1000000000}, {left / 1000000000, left % 1000000000}};
}
int create_fn(int clock, const Sigevent* e, GuestAddr* id) {
  if (clock != 0 && clock != 1 && clock != 7) return failure(err::einval);
  if (!id) return failure(err::efault);
  if (e && ((e->notify != 0 && e->notify != 1 && e->notify != 2) ||
            (e->notify == 0 && (e->signo < 1 || e->signo > 64)) ||
            (e->notify == 2 && (!e->function || e->attr))))
    return failure(err::einval);
  auto& s = service();
  std::lock_guard lock(s.mutex);
  static GuestAddr next = 16;
  auto value = next++;
  Timer t;
  t.clock = clock;
  t.event = e ? *e : Sigevent{value, 14, 0, 0, 0, {}};
  s.timers[value] = t;
  *id = value;
  return 0;
}
int delete_fn(GuestAddr id) {
  auto& s = service();
  std::lock_guard lock(s.mutex);
  if (!s.timers.erase(id)) return failure(err::einval);
  s.cv.notify_all();
  return 0;
}
int get_fn(GuestAddr id, Itimer* out) {
  if (!out) return failure(err::efault);
  auto& s = service();
  std::lock_guard lock(s.mutex);
  auto i = s.timers.find(id);
  if (i == s.timers.end()) return failure(err::einval);
  get_locked(i->second, out);
  return 0;
}
int set_fn(GuestAddr id, int flags, const Itimer* value, Itimer* old) {
  if (!value || flags & ~1) return failure(err::einval);
  for (auto& t : {value->interval, value->value})
    if (t.sec < 0 || t.nsec < 0 || t.nsec >= 1000000000 ||
        t.sec > (INT64_MAX - t.nsec) / 1000000000)
      return failure(err::einval);
  auto& s = service();
  std::lock_guard lock(s.mutex);
  auto i = s.timers.find(id);
  if (i == s.timers.end()) return failure(err::einval);
  auto& timer = i->second;
  auto next = value->value.sec * 1000000000 + value->value.nsec;
  auto now = timer.clock == 0 ? realtime_ns() : monotonic_ns();
  if (next && !(flags & 1)) {
    if (next > INT64_MAX - now) return failure(err::eoverflow);
    next += now;
  }
  if (old) get_locked(timer, old);
  timer.period = value->interval.sec * 1000000000 + value->interval.nsec;
  timer.next = next;
  timer.overrun = 0;
  s.cv.notify_all();
  return 0;
}
int overrun_fn(GuestAddr id) {
  auto& s = service();
  std::lock_guard lock(s.mutex);
  auto i = s.timers.find(id);
  return i == s.timers.end() ? failure(err::einval) : i->second.overrun;
}
GuestAddr real_timer() {
  static GuestAddr id = [] {
    GuestAddr value = 0;
    create_fn(0, nullptr, &value);
    return value;
  }();
  return id;
}
struct Itimerval {
  Timeval interval, value;
};
int setitimer_fn(int which, const Itimerval* in, Itimerval* old) {
  if (which != 0) return failure(err::enotsup);
  if (!in || in->interval.usec < 0 || in->interval.usec >= 1000000 || in->value.usec < 0 ||
      in->value.usec >= 1000000)
    return failure(err::einval);
  Itimer v{{in->interval.sec, in->interval.usec * 1000}, {in->value.sec, in->value.usec * 1000}},
      o{};
  int r = set_fn(real_timer(), 0, &v, old ? &o : nullptr);
  if (!r && old)
    *old = {{o.interval.sec, o.interval.nsec / 1000}, {o.value.sec, o.value.nsec / 1000}};
  return r;
}
int getitimer_fn(int which, Itimerval* out) {
  if (which != 0) return failure(err::enotsup);
  if (!out) return failure(err::efault);
  Itimer o{};
  int r = get_fn(real_timer(), &o);
  if (!r) *out = {{o.interval.sec, o.interval.nsec / 1000}, {o.value.sec, o.value.nsec / 1000}};
  return r;
}
unsigned alarm_fn(unsigned seconds) {
  Itimer v{{0, 0}, {seconds, 0}}, old{};
  set_fn(real_timer(), 0, &v, &old);
  return static_cast<unsigned>(old.value.sec + (old.value.nsec != 0));
}
}  // namespace
void register_timers(HleLibrary& lib) {
#define B(n, f) VENTURE_LIBC_BIND(lib, n, f)
  B("timer_create", create_fn);
  B("timer_delete", delete_fn);
  B("timer_gettime", get_fn);
  B("timer_settime", set_fn);
  B("timer_getoverrun", overrun_fn);
  B("getitimer", getitimer_fn);
  B("setitimer", setitimer_fn);
  B("alarm", alarm_fn);
#undef B
}
}  // namespace venture::hle::libc
