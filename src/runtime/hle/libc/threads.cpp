#include "vfs.h"
#include "abi32.h"
#include "condition_waiter.h"
#include "yield_backoff.h"
#include "core/hle_runtime.h"
#include "core/thread_api.h"
#include "core/thread_runtime.h"
#include "core/libc_lifecycle.h"
#include "core/signals.h"
#include "core/mutex_watch.h"
#include <type_traits>
#include "core/condition_census.h"
#include "core/profile.h"
#include "core/wait.h"
#include "common/environment.h"
#include "platform/cpu_affinity.h"
#include "platform/thread_priority.h"
#include <array>
#include <cstddef>
#include <bit>
#include <climits>
#include <optional>
#include <cstring>
#include <cstdio>

namespace venture::hle::libc {
void register_threads32_adapters();
bool try_mutex32(CallContext&, bool unlock);
namespace {
// Sizes and static type bits: bionic bits/pthread_types.h, pthread_mutex.cpp.
struct Mutex {
  std::uint32_t state, owner, depth, reserved[7];
};
struct Cond {
  std::uint32_t sequence, waiters, reserved[10];
};
static_assert(sizeof(Cond) == 48);
struct Rwlock {
  // waiters counts threads between registering and leaving a sequence wait, so
  // uncontended releases skip the futex bucket. Zero in PTHREAD_RWLOCK_INITIALIZER.
  std::uint32_t state, sequence, writers, owner, waiters, reserved[9];
};
struct Barrier {
  std::uint32_t arrivals, sequence, count, reserved[5];
};
struct Sem {
  std::uint32_t count, reserved[3];
};
struct Attr {
  std::uint32_t flags, pad;
  GuestAddr stack;
  std::uint64_t size, guard;
  std::int32_t policy, priority;
  char reserved[16];
};
static_assert(sizeof(Mutex) == 40 && sizeof(Cond) == 48 && sizeof(Rwlock) == 56);
static_assert(sizeof(Barrier) == 32 && sizeof(Sem) == 16 && sizeof(Attr) == 56);
auto atom(std::uint32_t& word) { return std::atomic_ref<std::uint32_t>(word); }
std::uint32_t tid() { return static_cast<std::uint32_t>(GuestThread::current()->tid()); }
int wait(std::uint32_t& word, std::uint32_t value, std::int64_t end = -1) {
  return futex_wait(*GuestThread::current(), h2g(&word), value, end);
}
template<class Call> auto watch_mutex(Mutex* m, const char* operation, Call call) {
  if (!shutdown_trace::mutex_watch_enabled) return call();
  shutdown_trace::trace_mutex_event(m, operation, INT_MIN);
  try {
    auto result = call();
    const int status = [&] {
      if constexpr (std::is_same_v<decltype(result), bool>) return result ? 0 : INT_MAX;
      else return result;
    }();
    shutdown_trace::trace_mutex_event(m, operation, status);
    return result;
  } catch (...) {
    shutdown_trace::trace_mutex_event(m, operation, INT_MIN + 1);
    throw;
  }
}
int mutex_init(Mutex* m, const std::uint64_t* attr) {
  return watch_mutex(m, "init", [&]() -> int {
    if (!m) return err::einval;
    *m = {};
    m->state = attr ? static_cast<std::uint32_t>(*attr & 3) << 14 : 0;
    return 0;

  });
}
int mutex_acquire(Mutex* m, std::int64_t end, bool attempt, bool deliver_signals = true) {
  return watch_mutex(m, attempt ? "trylock" : end >= 0 ? "timed_lock" : "lock", [&]() -> int {
    if (!m) return err::einval;
    auto state = atom(m->state);
    auto value = state.load(std::memory_order_relaxed);
    bool contended = false;
    for (;;) {
      if (value == 0xffff) return err::einval;
      if (!(value & 3)) {
        if (state.compare_exchange_weak(value, value | (contended ? 2 : 1),
                                        std::memory_order_acquire)) {
          atom(m->owner).store(tid(), std::memory_order_relaxed);
          m->depth = 1;
          return 0;
        }
        continue;
      }
      int type = (value >> 14) & 3;
      if (type && atom(m->owner).load(std::memory_order_relaxed) == tid()) {
        if (type == 1) {
          if (m->depth == UINT_MAX) return err::eagain;
          ++m->depth;
          return 0;
        }
        return attempt ? err::ebusy : err::edeadlk;
      }
      if (attempt) return err::ebusy;
      contended = true;
      // The contended bit is retained by the owner until release, ensuring a wake.
      if ((value & 3) != 2 &&
          !state.compare_exchange_weak(value, (value & ~3u) | 2, std::memory_order_relaxed))
        continue;
      value = (value & ~3u) | 2;
      int r = futex_wait(*GuestThread::current(), h2g(&m->state), value, end, ~0u, deliver_signals);
      if (r == -err::etimedout) return err::etimedout;
      value = state.load(std::memory_order_relaxed);
    }

  });
}
int mutex_lock(Mutex* m) { return mutex_acquire(m, -1, false); }
int mutex_try(Mutex* m) { return mutex_acquire(m, -1, true); }
int mutex_clock(Mutex* m, int clock, const Timespec* time) {
  std::int64_t end;
  int r = deadline(time, clock, &end);
  return r ? -r : mutex_acquire(m, end, false);
}
int mutex_timed(Mutex* m, const Timespec* t) { return mutex_clock(m, 0, t); }
int mutex_unlock(Mutex* m) {
  return watch_mutex(m, "unlock", [&]() -> int {
    auto state = atom(m->state);
    auto value = state.load(std::memory_order_relaxed);
    if (!(value & 3) || value == 0xffff) return err::eperm;
    if ((value & 0xc000) && atom(m->owner).load() != tid()) return err::eperm;
    if ((value & 0xc000) == 0x4000 && --m->depth) return 0;
    atom(m->owner).store(0, std::memory_order_relaxed);
    value = state.exchange(value & ~3u, std::memory_order_release);
    if ((value & 3) == 2) futex_wake(h2g(&m->state), 1, sync_census::WakeSource::mutex);
    return 0;

  });
}
bool try_mutex(CallContext& ctx, bool unlock) {
  if (ctx.cpu().isa() == GuestIsa::a32) return try_mutex32(ctx, unlock);
  auto* m = static_cast<Mutex*>(g2h(ctx.x(0)));
  if (!m) return false;
  return watch_mutex(m, unlock ? "fast_unlock" : "fast_lock", [&]() -> bool {
    auto state = atom(m->state);
    std::uint32_t expected = unlock ? 1 : 0;
    if (unlock) {
      if (state.load(std::memory_order_relaxed) != 1) return false;
      // Clear before the release CAS: a subsequent owner must retain its tid.
      // If a waiter changes 1 to 2, the original unlock performs its futex wake.
      const auto owner = atom(m->owner).load(std::memory_order_relaxed);
      atom(m->owner).store(0, std::memory_order_relaxed);
      if (!state.compare_exchange_strong(expected, 0, std::memory_order_release)) {
        atom(m->owner).store(owner, std::memory_order_relaxed);
        return false;
      }
    } else {
      if (!state.compare_exchange_strong(expected, 1, std::memory_order_acquire)) return false;
      atom(m->owner).store(static_cast<std::uint32_t>(ctx.thread().tid()), std::memory_order_relaxed);
      m->depth = 1;
    }
    ctx.return_x(0);
    return true;
  });
}
bool try_mutex_lock(CallContext& ctx) { return try_mutex(ctx, false); }
bool try_mutex_unlock(CallContext& ctx) { return try_mutex(ctx, true); }
int mutex_destroy(Mutex* m) {
  return watch_mutex(m, "destroy", [&]() -> int {
    auto value = atom(m->state).load();
    if (value & 3) return err::ebusy;
    return atom(m->state).compare_exchange_strong(value, 0xffff) ? 0 : err::ebusy;

  });
}
int cond_init(Cond* c, const std::uint64_t* a) {
  *c = {};
  c->sequence = a && (*a & 2) ? 2 : 0;
  return 0;
}
int cond_destroy(Cond*) { return 0; }
int cond_wake(Cond* c, int count) {
  const bool skipped = !atom(c->waiters).load();
  if (sync_census::enabled)
    sync_census::condition_notification(GuestThread::current() ? GuestThread::current()->tid() : 0,
                                        count == INT_MAX, skipped);
  if (skipped) return 0;
  atom(c->sequence).fetch_add(4, std::memory_order_release);
  futex_wake(h2g(c), count, sync_census::WakeSource::condition);
  return 0;
}
int cond_signal(Cond* c) { return cond_wake(c, 1); }
int cond_broadcast(Cond* c) { return cond_wake(c, INT_MAX); }
int cond_clock(Cond* c, Mutex* m, int clock, const Timespec* time) {
  return watch_mutex(m, "cond_wait", [&]() -> int {
    shutdown_trace::WaitScope wait_trace(*GuestThread::current(), shutdown_trace::Wait::condition, h2g(c), h2g(m));
    std::int64_t end = -1;
    if (time) {
      int r = deadline(time, clock, &end);
      if (r) return -r;
    }
    // Register before reading the sequence so signals without the predicate mutex
    // retain bionic's wake-before-park window. Retire before mutex reacquisition.
    std::optional<detail::ConditionWaiter> registration;
    registration.emplace(c->sequence, c->waiters);
    int r = mutex_unlock(m);
    if (r) return r;
    // Returning handlers must run without the mutex: GC/suspend initiators can
    // hold it while waiting for acknowledgements. Guest unwind cleanup reacquires
    // it only when a throwing handler escapes this wait.
    auto& thread = *GuestThread::current();
    GuestThread::ConditionWaitCleanup cleanup{thread.condition_wait_cleanup,
        [](void* mutex) { (void)mutex_acquire(static_cast<Mutex*>(mutex), -1, false, false); },
        m, thread.cpu().sp() + (thread.cpu().isa() == GuestIsa::a32 ? 8 : 16)};
    thread.condition_wait_cleanup = &cleanup;
    struct Restore {
      GuestThread& thread; GuestThread::ConditionWaitCleanup& cleanup;
      ~Restore() { thread.condition_wait_cleanup = cleanup.previous; }
    } restore{thread, cleanup};
    r = futex_wait(thread, h2g(&c->sequence), registration->sequence, end);
    registration.reset();
    int lock = cleanup.pending ? mutex_acquire(m, -1, false) : 0;
    return lock ? lock
                : (r == -err::etimedout ? err::etimedout : 0);  // POSIX permits spurious wakeups.
  });
}
int cond_wait(Cond* c, Mutex* m) { return cond_clock(c, m, 0, nullptr); }
int cond_timed(Cond* c, Mutex* m, const Timespec* t) {
  return cond_clock(c, m, (atom(c->sequence).load() & 2) ? 1 : 0, t);
}
int cond_monotonic(Cond* c, Mutex* m, const Timespec* t) { return cond_clock(c, m, 1, t); }
int rw_init(Rwlock* r, const std::uint64_t*) {
  *r = {};
  return 0;
}
int rw_destroy(Rwlock* r) { return atom(r->state).load() ? err::ebusy : 0; }
// Pairs with the seq_cst waiters increment in rw_acquire: either the releaser
// sees the waiter or the waiter's futex compare sees the new sequence.
void rw_release(Rwlock* r) {
  atom(r->sequence).fetch_add(1);
  if (atom(r->waiters).load()) futex_wake(h2g(&r->sequence), INT_MAX, sync_census::WakeSource::rwlock);
}
int rw_acquire(Rwlock* r, bool writer, bool attempt, std::int64_t end = -1) {
  if (writer) atom(r->writers).fetch_add(1);
  auto finish = [&](int error) {
    if (writer) {
      atom(r->writers).fetch_sub(1);
      rw_release(r);
    }
    return error;
  };
  for (;;) {
    auto seq = atom(r->sequence).load(std::memory_order_acquire);
    auto value = atom(r->state).load(std::memory_order_relaxed);
    // Bionic pthread_rwlock.cpp defaults to reader preference. Blocking a
    // recursive reader behind a pending writer would deadlock that reader.
    if (writer ? value == 0 : value != UINT_MAX) {
      if (value == UINT_MAX - 1) return finish(err::eagain);
      if (atom(r->state).compare_exchange_weak(value, writer ? UINT_MAX : value + 1,
                                               std::memory_order_acquire)) {
        if (writer) atom(r->owner).store(tid());
        return finish(0);
      }
      continue;
    }
    if (attempt) return finish(err::ebusy);
    if (value == UINT_MAX && atom(r->owner).load() == tid()) return finish(err::edeadlk);
    atom(r->waiters).fetch_add(1);
    const int waited = wait(r->sequence, seq, end);
    atom(r->waiters).fetch_sub(1);
    if (waited == -err::etimedout) return finish(err::etimedout);
  }
}
int rw_read(Rwlock* r) { return rw_acquire(r, false, false); }
int rw_write(Rwlock* r) { return rw_acquire(r, true, false); }
int rw_tryread(Rwlock* r) { return rw_acquire(r, false, true); }
int rw_trywrite(Rwlock* r) { return rw_acquire(r, true, true); }
int rw_timed(Rwlock* r, const Timespec* t, bool writer) {
  std::int64_t end;
  int e = deadline(t, 0, &end);
  return e ? -e : rw_acquire(r, writer, false, end);
}
int rw_timedread(Rwlock* r, const Timespec* t) { return rw_timed(r, t, false); }
int rw_timedwrite(Rwlock* r, const Timespec* t) { return rw_timed(r, t, true); }
int rw_unlock(Rwlock* r) {
  auto value = atom(r->state).load();
  if (value == UINT_MAX) {
    if (atom(r->owner).load() != tid()) return err::eperm;
    atom(r->owner).store(0);
    atom(r->state).store(0, std::memory_order_release);
  } else {
    do {
      if (!value) return err::eperm;
    } while (!atom(r->state).compare_exchange_weak(value, value - 1, std::memory_order_release));
  }
  rw_release(r);
  return 0;
}
int spin_init(std::uint64_t* s, int) {
  *s = 0;
  return 0;
}
int spin_destroy(std::uint64_t*) { return 0; }
int spin_try(std::uint64_t* s) {
  return std::atomic_ref(*s).exchange(1, std::memory_order_acquire) ? err::ebusy : 0;
}
int spin_lock(std::uint64_t* s) {
  while (spin_try(s)) host::yield();
  return 0;
}
int spin_unlock(std::uint64_t* s) {
  std::atomic_ref(*s).store(0, std::memory_order_release);
  return 0;
}
int barrier_init(Barrier* b, const int*, unsigned count) {
  if (!count) return err::einval;
  *b = {};
  b->count = count;
  return 0;
}
int barrier_destroy(Barrier* b) { return atom(b->arrivals).load() % b->count ? err::ebusy : 0; }
int barrier_wait(Barrier* b) {
  auto seq = atom(b->sequence).load(std::memory_order_acquire);
  auto arrival = atom(b->arrivals).fetch_add(1, std::memory_order_acq_rel) + 1;
  if (arrival % b->count == 0) {
    atom(b->sequence).fetch_add(1, std::memory_order_release);
    futex_wake(h2g(&b->sequence), INT_MAX, sync_census::WakeSource::barrier);
    return -1;
  }
  while (atom(b->sequence).load(std::memory_order_acquire) == seq) wait(b->sequence, seq);
  return 0;
}
int sem_init(Sem* s, int shared, unsigned count) {
  if (shared) return failure(err::enosys);
  if (count > INT_MAX) return failure(err::einval);
  *s = {};
  s->count = count;
  return 0;
}
int sem_destroy(Sem*) { return 0; }
int sem_value(Sem* s, int* value) {
  *value = static_cast<int>(atom(s->count).load());
  return 0;
}
int sem_post(Sem* s) {
  auto n = atom(s->count).load();
  do {
    if (n == INT_MAX) return failure(err::eoverflow);
  } while (!atom(s->count).compare_exchange_weak(n, n + 1, std::memory_order_release));
  futex_wake(h2g(s), 1, sync_census::WakeSource::semaphore);
  return 0;
}
int sem_acquire(Sem* s, bool attempt, std::int64_t end = -1) {
  for (;;) {
    auto n = atom(s->count).load(std::memory_order_relaxed);
    if (n) {
      if (atom(s->count).compare_exchange_weak(n, n - 1, std::memory_order_acquire)) return 0;
      continue;
    }
    if (attempt) return failure(err::eagain);
    int r = wait(s->count, 0, end);
    if (r == -err::eintr || r == -err::etimedout) return static_cast<int>(result(r));
  }
}
int sem_wait(Sem* s) { return sem_acquire(s, false); }
int sem_try(Sem* s) { return sem_acquire(s, true); }
int sem_clock(Sem* s, int clock, const Timespec* t) {
  std::int64_t end;
  int r = deadline(t, clock, &end);
  return r ? static_cast<int>(result(r)) : sem_acquire(s, false, end);
}
int sem_timed(Sem* s, const Timespec* t) { return sem_clock(s, 0, t); }
int once(std::uint32_t* state, GuestAddr fn) {
  auto a = atom(*state);
  for (;;) {
    auto value = a.load(std::memory_order_acquire);
    if (value == 2) return 0;
    if (!value && a.compare_exchange_weak(value, 1, std::memory_order_acquire)) {
      try {
        GuestThread::current()->call(fn, {});
      } catch (...) {
        a.store(0, std::memory_order_release);
        futex_wake(h2g(state), INT_MAX, sync_census::WakeSource::once);
        throw;
      }
      a.store(2, std::memory_order_release);
      futex_wake(h2g(state), INT_MAX, sync_census::WakeSource::once);
      return 0;
    }
    wait(*state, 1);
  }
}
struct Key {
  // bionic libc/bionic/pthread_key.cpp: bit zero is live, and each create or
  // delete advances the sequence. Readers compare it with owner-thread data.
  std::atomic<std::uint64_t> sequence{0};
  GuestAddr destructor = 0;
};
std::mutex key_mutex;
std::array<Key, 256> keys;
int key_create(int* key, GuestAddr destructor) {
  std::lock_guard lock(key_mutex);
  for (unsigned i = 0; i < keys.size(); ++i)
    if (!(keys[i].sequence.load(std::memory_order_relaxed) & 1)) {
      auto& k = keys[i];
      k.destructor = destructor;
      k.sequence.fetch_add(1, std::memory_order_release);
      *key = i;
      return 0;
    }
  return err::eagain;
}
int key_delete(int key) {
  std::lock_guard lock(key_mutex);
  if (key < 0 || key >= 256 || !(keys[key].sequence.load(std::memory_order_relaxed) & 1)) return err::einval;
  keys[key].sequence.fetch_add(1, std::memory_order_release);
  return 0;
}
int set_specific(CallContext& c, int key, GuestAddr value) {
  if (key < 0 || key >= 256) return err::einval;
  const auto sequence = keys[key].sequence.load(std::memory_order_acquire);
  if (!(sequence & 1)) return err::einval;
  c.thread().pthread_values[key] = {sequence, value};
  return 0;
}
GuestAddr get_specific(CallContext& c, int key) {
  if (key < 0 || key >= 256) return 0;
  const auto sequence = keys[key].sequence.load(std::memory_order_acquire);
  auto& data = c.thread().pthread_values[key];
  if ((sequence & 1) && data.sequence == sequence) return data.value;
  data.value = 0;
  return 0;
}
// A64 guest code for pthread_self and the key accessors, matching the handlers
// above: a host call costs several times the lookup, and pthread_getspecific is
// the most frequent libc call in Batman and North Star (0.6-1.1 M/s). keys is
// host memory read through identity fastmem; TLS slot 1 holds the handle, and
// the thread's key data follows the handle page (core/thread.h).
static_assert(sizeof(Key) == 16 && offsetof(Key, sequence) == 0 && keys.size() == GuestThread::kPthreadKeyCount);
static_assert(sizeof(GuestThread::PthreadKeyValue) == 16 && GuestThread::kPthreadKeyValuesOffset == 1 << 12);
constexpr std::array<std::uint32_t, 20> kGetSpecific64{
    0x7104001f, // cmp w0, #256
    0x54000222, // b.hs zero
    0, 0, 0, 0, // mov x16, keys
    0x8b205210, // add x16, x16, w0, uxtw #4
    0xc8dffe11, // ldar x17, [x16]
    0xd53bd050, // mrs x16, tpidr_el0
    0xf9400610, // ldr x16, [x16, #8]
    0x91400610, // add x16, x16, #1, lsl #12
    0x8b205210, // add x16, x16, w0, uxtw #4
    0xa9400209, // ldp x9, x0, [x16]
    0x36000091, // tbz w17, #0, clear
    0xeb11013f, // cmp x9, x17
    0x54000041, // b.ne clear
    0xd65f03c0, // ret
    0xf900061f, // clear: str xzr, [x16, #8]
    0xd2800000, // zero: mov x0, #0
    0xd65f03c0, // ret
};
constexpr std::array<std::uint32_t, 18> kSetSpecific64{
    0x7104001f, // cmp w0, #256
    0x540001e2, // b.hs invalid
    0, 0, 0, 0, // mov x16, keys
    0x8b205210, // add x16, x16, w0, uxtw #4
    0xc8dffe11, // ldar x17, [x16]
    0x36000111, // tbz w17, #0, invalid
    0xd53bd050, // mrs x16, tpidr_el0
    0xf9400610, // ldr x16, [x16, #8]
    0x91400610, // add x16, x16, #1, lsl #12
    0x8b205210, // add x16, x16, w0, uxtw #4
    0xa9000611, // stp x17, x1, [x16]
    0x52800000, // mov w0, #0
    0xd65f03c0, // ret
    0x528002c0, // invalid: mov w0, #EINVAL
    0xd65f03c0, // ret
};
static_assert(err::einval == 22);
constexpr std::array<std::uint32_t, 3> kSelf64{
    0xd53bd040, // mrs x0, tpidr_el0
    0xf9400400, // ldr x0, [x0, #8]
    0xd65f03c0, // ret
};
template<std::size_t N>
GuestAddr key_routine(std::array<std::uint32_t, N> code, std::string_view name) {
  const auto base = reinterpret_cast<std::uint64_t>(&keys[0].sequence);
  for (unsigned hw = 0; hw < 4; ++hw)  // movz, then movk x16, #part, lsl #(16 * hw)
    code[2 + hw] = (hw ? 0xf2800010u : 0xd2800010u) | hw << 21 | static_cast<std::uint32_t>(base >> 16 * hw & 0xffff) << 5;
  return code_stub(code, name);
}
int attr_init(Attr* a) {
  *a = {};
  a->size = 1024 * 1024;
  a->guard = 4096;
  return 0;
}
int attr_destroy(Attr*) { return 0; }
int attr_setstack(Attr* a, GuestAddr base, std::uint64_t size) {
  if (size < 16384) return err::einval;
  a->stack = base;
  a->size = size;
  return 0;
}
int attr_getstack(const Attr* a, GuestAddr* base, std::uint64_t* size) {
  *base = a->stack;
  *size = a->size;
  return 0;
}
int attr_setsize(Attr* a, std::uint64_t size) {
  if (size < 16384) return err::einval;
  a->size = size;
  return 0;
}
int attr_getsize(const Attr* a, std::uint64_t* size) {
  *size = a->size;
  return 0;
}
int attr_setguard(Attr* a, std::uint64_t size) {
  a->guard = size;
  return 0;
}
int attr_getguard(const Attr* a, std::uint64_t* size) {
  *size = a->guard;
  return 0;
}
int attr_setdetach(Attr* a, int value) {
  if (value < 0 || value > 1) return err::einval;
  a->flags = (a->flags & ~1u) | value;
  return 0;
}
int attr_getdetach(const Attr* a, int* value) {
  *value = a->flags & 1;
  return 0;
}
int attr_setpolicy(Attr* a, int value) {
  if (value < 0 || value > 2) return err::einval;
  a->policy = value;
  return 0;
}
int attr_getpolicy(const Attr* a, int* value) {
  *value = a->policy;
  return 0;
}
int attr_setparam(Attr* a, const int* value) {
  a->priority = *value;
  return 0;
}
int attr_getparam(const Attr* a, int* value) {
  *value = a->priority;
  return 0;
}
int attr_setinherit(Attr* a, int value) {
  if (value < 0 || value > 1) return err::einval;
  a->flags = (a->flags & ~12u) | (value ? 4u : 8u);
  return 0;
}
int attr_getinherit(const Attr* a, int* value) {
  // Bionic retains a third, historical mode when neither flag was set.
  *value = (a->flags & 4u) ? 1 : (a->flags & 8u) ? 0 : a->policy == 0;
  return 0;
}
int create(GuestAddr* handle, const Attr* a, GuestAddr start, GuestAddr arg) {
  threads::Options o;
  if (a) {
    o.stack = a->stack;
    o.stack_size = a->size;
    o.guard_size = a->guard;
    o.detached = a->flags & 1;
    // Before explicit/inherit attributes were added, Bionic only applied
    // scheduling parameters for a non-default policy. A priority stored with
    // SCHED_OTHER in that historical mode must not prevent thread creation.
    const bool apply_schedule = !(a->flags & 4u) && ((a->flags & 8u) || a->policy);
    if (apply_schedule) {
      if (!a->policy && a->priority) return err::einval;
      if (a->policy || a->priority) return err::enotsup;
    }
  }
  o.on_exit = run_key_destructors;
  return -threads::create(o, start, arg, handle);
}
int join(GuestAddr h, GuestAddr* value) {
  return -threads::join(*GuestThread::current(), h, value);
}
int detach(GuestAddr h) { return -threads::detach(h); }
void run_cleanup_handlers(GuestThread& thread);
void thread_exit(GuestAddr value) {
  auto& thread = *GuestThread::current();
  // bionic pops cleanup handlers inside pthread_exit, while the frames holding
  // their records are still live. Once the exit unwinds, the guest stack pointer
  // is back at the thread's start and handler calls would reuse that memory.
  run_cleanup_handlers(thread);
  threads::exit(thread, value);
}
GuestAddr self() { return GuestThread::current()->pthread_self; }
GuestAddr self_hle(CallContext& c) { return c.thread().pthread_self; }
int equal(GuestAddr a, GuestAddr b) { return a == b; }
int getattr(GuestAddr h, Attr* a) {
  threads::Options o;
  int r = threads::describe(h, &o, nullptr, nullptr);
  if (r) return -r;
  attr_init(a);
  a->stack = o.stack;
  a->size = o.stack_size;
  a->guard = o.guard_size;
  a->flags = o.detached;
  return 0;
}
int gettid_np(GuestAddr h) {
  std::int32_t id;
  int r = threads::describe(h, nullptr, &id, nullptr);
  return r ? -1 : id;
}
int gettid() { return GuestThread::current()->tid(); }
int gettid_hle(CallContext& c) { return c.thread().tid(); }
int setname(GuestAddr h, const char* s) {
  if (!s) return err::einval;
  if (std::strlen(s) > 15) return err::erange;
  return -threads::set_name(h, s);
}
int getname(GuestAddr h, char* s, std::uint64_t size) {
  std::string name;
  int r = threads::describe(h, nullptr, nullptr, &name);
  if (r) return -r;
  if (size <= name.size()) return err::erange;
  std::memcpy(s, name.c_str(), name.size() + 1);
  return 0;
}
int thread_kill(GuestAddr h, int signal) {
  int id = gettid_np(h);
  if (id < 0) return err::esrch;
  return -signals::send_to_thread(id, {.signo = signal});
}
// Default 16:50 (yield_backoff.h; VENTURE_YIELD_BACKOFF=N:US, 0 disables): a thread's yields after
// N back-to-back ones (< 200 us apart) sleep US microseconds, as a yield on a busy device
// gives up its core. Host yields return at once on an idle desktop, so a guest's bounded
// yield retry (UE4 FTexture2DUpdate::DoConditionalLock: 99 yields) gives up within
// ~0.1-0.6 ms of a short lock hold and can orphan the update (Vader III quit hang).
bool yield_backoff() {
  static const auto config = yield_backoff_setting(environment::getenv("VENTURE_YIELD_BACKOFF"));
  if (!config.first) return false;
  thread_local std::int64_t last = 0;
  thread_local unsigned streak = 0;
  auto now = monotonic_ns();
  streak = now - last < 200000 ? streak + 1 : 0;
  last = now;
  if (streak < config.first) return false;
  auto deadline = now + std::int64_t{config.second} * 1000;
  sleep_until(*GuestThread::current(), deadline, deadline - now);
  last = monotonic_ns();
  return true;
}
int yield() {
  if (yield_backoff()) return 0;
  if (profile::futex_timing_enabled) {
    auto event = profile::scheduling_begin(*GuestThread::current(), 0, true);
    host::yield();
    profile::scheduling_end(event);
    return 0;
  }
  host::yield();
  return 0;
}
template<class Word>
std::int32_t cpucount(std::uint64_t size, const void* mask) {
  // bionic libc/bionic/sched_cpucount.c and libc/include/sched.h:
  // CPU_COUNT_S counts complete __CPU_BITTYPE words (64-bit A64, 32-bit A32),
  // including dynamically allocated masks larger than cpu_set_t.
  std::int32_t count = 0;
  for (std::uint64_t i = 0; i < size / sizeof(Word); ++i) {
    Word word;
    std::memcpy(&word, static_cast<const std::byte*>(mask) + i * sizeof(Word), sizeof(Word));
    count += std::popcount(word);
  }
  return count;
}
int getaffinity(int, std::uint64_t size, void* mask) {
  auto bytes = (cpu_count() + 7) / 8;
  if (size < bytes) return failure(err::einval);
  std::memset(mask, 0, size);
  for (unsigned i = 0; i < cpu_count(); ++i)
    static_cast<unsigned char*>(mask)[i / 8] |= 1u << (i % 8);
  return 0;
}
int getcpu() {
  const int cpu = platform::current_cpu();
  if (cpu < 0) return static_cast<int>(result(cpu));
  // /proc and sysconf expose a compact virtual CPU set (at most eight).
  // Project the host's current scheduler location into that same set.
  return static_cast<int>(static_cast<unsigned>(cpu) % cpu_count());
}
int setaffinity(int, std::uint64_t size, const void* mask) {
  if (!mask || !size) return failure(err::einval);
  for (std::uint64_t i = 0; i < size; ++i)
    if (static_cast<const unsigned char*>(mask)[i]) return 0;
  return failure(err::einval);
}
std::mutex atfork_mutex;
std::vector<std::array<GuestAddr, 4>> atfork_handlers;
// bionic pthread.h __pthread_cleanup_t: previous, routine, argument.
struct Cleanup {
  GuestAddr previous, routine, arg;
};
thread_local Cleanup* cleanup_top = nullptr;
struct Cleanup32 { std::uint32_t previous, routine, arg; };
thread_local Cleanup32* cleanup_top32 = nullptr;
void cleanup_push32(Cleanup32* c, GuestAddr fn, GuestAddr arg) {
  *c = {h2g32(cleanup_top32), static_cast<std::uint32_t>(fn), static_cast<std::uint32_t>(arg)};
  cleanup_top32 = c;
}
void cleanup_pop32(Cleanup32* c, int execute) {
  cleanup_top32 = g2h<Cleanup32>(c->previous);
  if (execute) GuestThread::current()->call(c->routine, {c->arg});
}
void cleanup_push(Cleanup* c, GuestAddr fn, GuestAddr arg) {
  *c = {h2g(cleanup_top), fn, arg};
  cleanup_top = c;
}
void cleanup_pop(Cleanup* c, int execute) {
  cleanup_top = g2h<Cleanup>(c->previous);
  if (execute) GuestThread::current()->call(c->routine, {c->arg});
}
void run_cleanup_handlers(GuestThread& thread) {
  while (cleanup_top32) {
    auto* c = cleanup_top32;
    cleanup_top32 = g2h<Cleanup32>(c->previous);
    thread.call(c->routine, {c->arg});
  }
  while (cleanup_top) {
    auto* c = cleanup_top;
    cleanup_top = g2h<Cleanup>(c->previous);
    thread.call(c->routine, {c->arg});
  }
}

int getsched(GuestAddr h, int* policy, int* priority) {
  std::int32_t target;
  int r = threads::describe(h, nullptr, &target, nullptr);
  if (r) return -r;
  if (!policy || !priority) return err::einval;
  return -platform::get_audio_thread_scheduler(target, policy, priority);
}
int setsched(GuestAddr h, int policy, const int* priority) {
  std::int32_t target;
  int r = threads::describe(h, nullptr, &target, nullptr);
  if (r) return -r;
  if (!priority || policy < 0 || policy > 2) return err::einval;
  if ((policy == 0 && *priority != 0) || (policy != 0 && (*priority < 1 || *priority > 99)))
    return err::einval;
  // AOSP audio SCHED_FIFO uses priorities 2 and 3; other RT classes stay denied.
  if (policy == 1 && (*priority == 2 || *priority == 3))
    return -platform::set_audio_thread_priority(target, *priority);
  if (policy || *priority) return err::eperm;
  return -platform::set_audio_thread_priority(target, 0);
}
int cpuclock(GuestAddr h, int* clock) {
  if (h != self()) return err::enotsup;
  *clock = 3;
  return 0;
}
int sched_priority_max(int policy) {
  if (policy < 0 || policy > 2) return failure(err::einval);
  return policy ? 99 : 0;
}
int sched_priority_min(int policy) {
  if (policy < 0 || policy > 2) return failure(err::einval);
  return policy ? 1 : 0;
}
int sched_param(int id, int* value) {
  if (id && id != 1000 && id != gettid()) return failure(err::esrch);
  *value = 0;
  return 0;
}
int sched_policy(int id) {
  int value;
  return sched_param(id, &value);
}
// An unprivileged app thread: the normal classes (OTHER, BATCH, IDLE) at
// priority 0 succeed and leave placement to the host scheduler; real-time
// classes are denied as on a headset (UE4's FRunnableThread priorities).
int sched_set(int id, int policy, const int* priority) {
  constexpr int reset_on_fork = 0x40000000;
  policy &= ~reset_on_fork;
  if (id < 0 || !priority || policy < 0 || policy > 5 || policy == 4) return failure(err::einval);
  if (policy == 1 || policy == 2) return failure(*priority < 1 || *priority > 99 ? err::einval : err::eperm);
  return *priority ? failure(err::einval) : 0;
}
int sched_set_param(int id, const int* priority) {
  return id < 0 || !priority || *priority ? failure(err::einval) : 0;
}
int barrier_attr_init(int* a) {
  *a = 0;
  return 0;
}
int barrier_attr_destroy(int*) { return 0; }
int barrier_attr_set(int* a, int value) {
  if (value < 0 || value > 1) return err::einval;
  *a = value;
  return 0;
}
int barrier_attr_get(const int* a, int* value) {
  *value = *a;
  return 0;
}
int atfork(GuestAddr prepare, GuestAddr parent, GuestAddr child) {
  std::lock_guard lock(atfork_mutex);
  atfork_handlers.push_back({prepare, parent, child, 0});
  return 0;
}
int register_atfork(GuestAddr prepare, GuestAddr parent, GuestAddr child, GuestAddr dso) {
  std::lock_guard lock(atfork_mutex);
  atfork_handlers.push_back({prepare, parent, child, dso});
  return 0;
}
int small_init(std::uint64_t* a) {
  *a = 0;
  return 0;
}
int small_destroy(std::uint64_t*) { return 0; }
int mutex_type_set(std::uint64_t* a, int v) {
  if (v < 0 || v > 2) return err::einval;
  *a = (*a & ~3ull) | v;
  return 0;
}
int mutex_type_get(const std::uint64_t* a, int* v) {
  *v = *a & 3;
  return 0;
}
int shared_set(std::uint64_t* a, int v) {
  if (v < 0 || v > 1) return err::einval;
  *a = (*a & ~0x10ull) | (v << 4);
  return 0;
}
int shared_get(const std::uint64_t* a, int* v) {
  *v = (*a >> 4) & 1;
  return 0;
}
int clock_set(std::uint64_t* a, int v) {
  if (v != 0 && v != 1) return err::einval;
  *a = (*a & ~2ull) | (v << 1);
  return 0;
}
int clock_get(const std::uint64_t* a, int* v) {
  *v = (*a >> 1) & 1;
  return 0;
}
int mutex_type_get32(const std::uint32_t* a, int* v) { *v = *a & 3; return 0; }
int shared_get32(const std::uint32_t* a, int* v) { *v = (*a >> 4) & 1; return 0; }
int clock_get32(const std::uint32_t* a, int* v) { *v = (*a >> 1) & 1; return 0; }
}  // namespace
void run_key_destructors(GuestThread& thread) {
  // Normally empty here: pthread_exit has run them. A start routine that returns
  // with handlers still pushed leaves records in frames that no longer exist.
  run_cleanup_handlers(thread);
  run_guest_thread_destructors(thread);

  for (int pass = 0; pass < 4; ++pass)
    for (unsigned i = 0; i < keys.size(); ++i) {
      GuestAddr fn = 0, value = 0;
      {
        std::lock_guard lock(key_mutex);
        auto& data = thread.pthread_values[i];
        const auto sequence = keys[i].sequence.load(std::memory_order_relaxed);
        if ((sequence & 1) && sequence == data.sequence && data.value) {
          fn = keys[i].destructor;
          value = data.value;
          data.value = 0;
        }
      }
      if (fn) thread.call(fn, {value});
    }
}
void register_threads(HleLibrary& lib) {
  set_guest_thread_exit_handler(run_key_destructors);
  if (process_config().isa() == GuestIsa::a32) {
    register_threads32_adapters();
    // cpu_set_t differs in word size, but these implementations treat its
    // caller-sized storage as bytes. No LP64 layout conversion is needed.
    abi32::adapt<&getaffinity>("sched_getaffinity");
    abi32::adapt<&setaffinity>("sched_setaffinity");
#define A(n, f) abi32::adapt<&f>(n)
    // pthread_t is a low core-owned handle. OutLong/OutPtr staging in invoke
    // writes four bytes for join/key/create outputs, including their canaries.
    A("pthread_join", join); A("pthread_detach", detach); A("pthread_exit", thread_exit);
    A("pthread_self", self_hle); A("pthread_equal", equal); A("pthread_kill", thread_kill);
    A("pthread_gettid_np", gettid_np); A("pthread_setname_np", setname); A("pthread_getname_np", getname);
    A("pthread_key_create", key_create); A("pthread_once", once);
    A("pthread_atfork", atfork); A("__register_atfork", register_atfork);
    A("pthread_getschedparam", getsched); A("pthread_setschedparam", setsched); A("pthread_getcpuclockid", cpuclock);
    A("__pthread_cleanup_push", cleanup_push32); A("__pthread_cleanup_pop", cleanup_pop32);
    A("__sched_cpucount", cpucount<std::uint32_t>);
    A("pthread_mutexattr_gettype", mutex_type_get32); A("pthread_mutexattr_getpshared", shared_get32);
    A("pthread_condattr_getclock", clock_get32);
    A("pthread_spin_init", spin_init); A("pthread_spin_destroy", spin_destroy); A("pthread_spin_lock", spin_lock);
    A("pthread_spin_trylock", spin_try); A("pthread_spin_unlock", spin_unlock);
    A("pthread_barrier_init", barrier_init); A("pthread_barrier_destroy", barrier_destroy); A("pthread_barrier_wait", barrier_wait);
#undef A
  }
#define B(n, f) VENTURE_LIBC_BIND(lib, n, f)
  B("pthread_create", create);
  B("pthread_join", join);
  B("pthread_detach", detach);
  B("pthread_exit", thread_exit);
  add(lib, "pthread_self", wrap<&self_hle>(), Execution::InJit);
  add(lib, "pthread_equal", wrap<&equal>(), Execution::InJit);
  B("pthread_kill", thread_kill);
  B("pthread_getattr_np", getattr);
  B("pthread_gettid_np", gettid_np);
  B("pthread_setname_np", setname);
  B("pthread_getname_np", getname);
  B("pthread_key_create", key_create);
  B("pthread_key_delete", key_delete);
  add(lib, "pthread_getspecific", wrap<&get_specific>(), Execution::InJit);
  add(lib, "pthread_setspecific", wrap<&set_specific>(), Execution::InJit);
  B("pthread_once", once);
  B("pthread_attr_init", attr_init);
  B("pthread_attr_destroy", attr_destroy);
  B("pthread_attr_setstack", attr_setstack);
  B("pthread_attr_getstack", attr_getstack);
  B("pthread_attr_setstacksize", attr_setsize);
  B("pthread_attr_getstacksize", attr_getsize);
  B("pthread_attr_setguardsize", attr_setguard);
  B("pthread_attr_getguardsize", attr_getguard);
  B("pthread_attr_setdetachstate", attr_setdetach);
  B("pthread_attr_getdetachstate", attr_getdetach);
  B("pthread_attr_setschedpolicy", attr_setpolicy);
  B("pthread_attr_getschedpolicy", attr_getpolicy);
  B("pthread_attr_setschedparam", attr_setparam);
  B("pthread_attr_getschedparam", attr_getparam);
  B("pthread_attr_setinheritsched", attr_setinherit);
  B("pthread_attr_getinheritsched", attr_getinherit);
  B("pthread_mutex_init", mutex_init);
  B("pthread_mutex_destroy", mutex_destroy);
  add(lib, "pthread_mutex_lock", wrap<&mutex_lock>(), Execution::InJit, try_mutex_lock);
  add(lib, "pthread_mutex_trylock", wrap<&mutex_try>(), Execution::InJit);
  add(lib, "pthread_mutex_unlock", wrap<&mutex_unlock>(), Execution::InJit, try_mutex_unlock);
  B("pthread_mutex_timedlock", mutex_timed);
  B("pthread_mutex_clocklock", mutex_clock);
  B("pthread_cond_init", cond_init);
  B("pthread_cond_destroy", cond_destroy);
  B("pthread_cond_wait", cond_wait);
  B("pthread_cond_timedwait", cond_timed);
  B("pthread_cond_timedwait_monotonic_np", cond_monotonic);
  B("pthread_cond_clockwait", cond_clock);
  B("pthread_cond_signal", cond_signal);
  B("pthread_cond_broadcast", cond_broadcast);
  B("pthread_rwlock_init", rw_init);
  B("pthread_rwlock_destroy", rw_destroy);
  B("pthread_rwlock_rdlock", rw_read);
  B("pthread_rwlock_wrlock", rw_write);
  B("pthread_rwlock_tryrdlock", rw_tryread);
  B("pthread_rwlock_trywrlock", rw_trywrite);
  B("pthread_rwlock_unlock", rw_unlock);
  B("pthread_rwlock_timedrdlock", rw_timedread);
  B("pthread_rwlock_timedwrlock", rw_timedwrite);
  B("pthread_spin_init", spin_init);
  B("pthread_spin_destroy", spin_destroy);
  B("pthread_spin_lock", spin_lock);
  B("pthread_spin_trylock", spin_try);
  B("pthread_spin_unlock", spin_unlock);
  B("pthread_barrier_init", barrier_init);
  B("pthread_barrier_destroy", barrier_destroy);
  B("pthread_barrier_wait", barrier_wait);
  B("sem_init", sem_init);
  B("sem_destroy", sem_destroy);
  B("sem_getvalue", sem_value);
  B("sem_post", sem_post);
  B("sem_wait", sem_wait);
  B("sem_trywait", sem_try);
  B("sem_timedwait", sem_timed);
  B("sem_clockwait", sem_clock);
  B("pthread_mutexattr_init", small_init);
  B("pthread_mutexattr_destroy", small_destroy);
  B("pthread_mutexattr_settype", mutex_type_set);
  B("pthread_mutexattr_gettype", mutex_type_get);
  B("pthread_mutexattr_setpshared", shared_set);
  B("pthread_mutexattr_getpshared", shared_get);
  B("pthread_condattr_init", small_init);
  B("pthread_condattr_destroy", small_destroy);
  B("pthread_condattr_setclock", clock_set);
  B("pthread_condattr_getclock", clock_get);
  B("pthread_condattr_setpshared", shared_set);
  B("pthread_condattr_getpshared", shared_get);
  B("pthread_rwlockattr_init", small_init);
  B("pthread_rwlockattr_destroy", small_destroy);
  B("pthread_rwlockattr_setpshared", shared_set);
  B("pthread_rwlockattr_getpshared", shared_get);
  B("pthread_atfork", atfork);
  B("__register_atfork", register_atfork);
  B("sched_yield", yield);
  B("sched_getaffinity", getaffinity);
  B("sched_getcpu", getcpu);
  B("__sched_cpucount", cpucount<std::uint64_t>);
  B("sched_setaffinity", setaffinity);
  add(lib, "gettid", wrap<&gettid_hle>(), Execution::InJit);
  B("pthread_getschedparam", getsched);
  B("pthread_setschedparam", setsched);
  B("pthread_getcpuclockid", cpuclock);
  B("sched_get_priority_max", sched_priority_max);
  B("sched_get_priority_min", sched_priority_min);
  B("sched_getparam", sched_param);
  B("sched_getscheduler", sched_policy);
  B("sched_setscheduler", sched_set);
  B("sched_setparam", sched_set_param);
  B("__pthread_cleanup_push", cleanup_push);
  B("__pthread_cleanup_pop", cleanup_pop);
  B("pthread_barrierattr_init", barrier_attr_init);
  B("pthread_barrierattr_destroy", barrier_attr_destroy);
  B("pthread_barrierattr_setpshared", barrier_attr_set);
  B("pthread_barrierattr_getpshared", barrier_attr_get);
#undef B
  if (process_config().isa() == GuestIsa::a64) {
    // The handlers stay registered for direct dispatch; the exports run guest code.
    lib.add_function("pthread_self", code_stub(kSelf64, "pthread_self"));
    lib.add_function("pthread_getspecific", key_routine(kGetSpecific64, "pthread_getspecific"));
    lib.add_function("pthread_setspecific", key_routine(kSetSpecific64, "pthread_setspecific"));
  }
}
}  // namespace venture::hle::libc
