#include "vfs.h"
#include "abi32.h"
#include "core/thread_api.h"
#include <climits>

namespace venture::hle::libc {
namespace {
using abi32::Timespec;
// bionic libc/include/bits/pthread_types.h (NDK r30 ARM): mutex/cond/attrs
// are one word, pthread_attr_t is 24 bytes and rwlock is 40 bytes.
struct Attr { std::uint32_t flags, stack, size, guard; std::int32_t policy, priority; };
struct Rwlock { std::uint32_t state, sequence, owner, reserved[7]; };
static_assert(sizeof(Attr) == 24 && sizeof(Rwlock) == 40);
auto atom(std::uint32_t& value) { return std::atomic_ref(value); }
int wait(std::uint32_t& word, std::uint32_t value, std::int64_t end = -1) {
  return futex_wait(*GuestThread::current(), h2g(&word), value, end);
}
int deadline32(const Timespec* t, int clock, std::int64_t* end) {
  if (!t) return err::einval;
  venture::hle::libc::Timespec wide{t->sec, t->nsec};
  return -deadline(&wide, clock, end);
}
// bionic pthread_mutex.cpp: state bits 0..1, recursion bits 2..12,
// shared bit 13, type bits 14..15, owner bits 16..31. The owner is a
// runtime live-thread index, not a truncated Linux tid.
constexpr std::uint32_t count_mask = 0x1ffc, type_mask = 0xc000, attr_mask = 0xe000;
std::uint32_t owner() { return std::uint32_t(GuestThread::current()->mutex_owner_index()) << 16; }
int mutex_init(std::uint32_t* m, const std::uint32_t* a) {
  if (!m) return err::einval;
  if (a && (*a & 0x10)) return err::enotsup;
  *m = a ? (*a & 3) << 14 : 0;
  return 0;
}
int mutex_acquire(std::uint32_t* m, std::int64_t end, bool attempt, bool deliver_signals = true) {
  if (!m) return err::einval;
  auto state = atom(*m);
  auto value = state.load(std::memory_order_relaxed);
  bool contended = false;
  const auto me = owner();
  for (;;) {
    if (value == 0xffff || (value & type_mask) == type_mask) return err::einval;
    if (!(value & 3)) {
      auto next = (value & attr_mask) | me | (contended ? 2u : 1u);
      if (state.compare_exchange_weak(value, next, std::memory_order_acquire)) return 0;
      continue;
    }
    if ((value & type_mask) && (value & 0xffff0000) == me) {
      if ((value & type_mask) != 0x4000) return attempt ? err::ebusy : err::edeadlk;
      if ((value & count_mask) == count_mask) return err::eagain;
      if (state.compare_exchange_weak(value, value + 4, std::memory_order_relaxed)) return 0;
      continue;
    }
    if (attempt) return err::ebusy;
    contended = true;
    if ((value & 3) != 2 &&
        !state.compare_exchange_weak(value, (value & ~3u) | 2, std::memory_order_relaxed)) continue;
    value = (value & ~3u) | 2;
    if (futex_wait(*GuestThread::current(), h2g(m), value, end, ~0u, deliver_signals) == -err::etimedout) return err::etimedout;
    value = state.load(std::memory_order_relaxed);
  }
}
int mutex_lock(std::uint32_t* m) { return mutex_acquire(m, -1, false); }
int mutex_try(std::uint32_t* m) { return mutex_acquire(m, -1, true); }
int mutex_clock(std::uint32_t* m, int clock, const Timespec* t) {
  std::int64_t end;
  int r = deadline32(t, clock, &end);
  return r ? r : mutex_acquire(m, end, false);
}
int mutex_timed(std::uint32_t* m, const Timespec* t) { return mutex_clock(m, 0, t); }
int mutex_unlock(std::uint32_t* m) {
  if (!m) return err::einval;
  auto state = atom(*m);
  auto value = state.load(std::memory_order_relaxed);
  for (;;) {
    if (!(value & 3) || value == 0xffff) return err::eperm;
    if ((value & type_mask) && (value & 0xffff0000) != owner()) return err::eperm;
    if ((value & type_mask) == 0x4000 && (value & count_mask)) {
      if (state.compare_exchange_weak(value, value - 4, std::memory_order_relaxed)) return 0;
    } else if (state.compare_exchange_weak(value, value & attr_mask, std::memory_order_release)) {
      if ((value & 3) == 2) futex_wake(h2g(m), 1, sync_census::WakeSource::mutex);
      return 0;
    }
  }
}
int mutex_destroy(std::uint32_t* m) {
  auto value = atom(*m).load();
  if (value & 3) return err::ebusy;
  return atom(*m).compare_exchange_strong(value, 0xffff) ? 0 : err::ebusy;
}
int cond_init(std::uint32_t* c, const std::uint32_t* a) {
  if (a && (*a & 1)) return err::enotsup;
  *c = a ? *a & 2 : 0; return 0;
}
int cond_destroy(std::uint32_t*) { return 0; }
int cond_wake(std::uint32_t* c, int count) {
  atom(*c).fetch_add(4, std::memory_order_release); futex_wake(h2g(c), count, sync_census::WakeSource::condition); return 0;
}
int cond_signal(std::uint32_t* c) { return cond_wake(c, 1); }
int cond_broadcast(std::uint32_t* c) { return cond_wake(c, INT_MAX); }
int cond_clock(std::uint32_t* c, std::uint32_t* m, int clock, const Timespec* t) {
  std::int64_t end = -1;
  if (t) { auto r = deadline32(t, clock, &end); if (r) return r; }
  auto seq = atom(*c).load(std::memory_order_acquire);
  auto r = mutex_unlock(m); if (r) return r;
  // Suspend/GC handlers must acknowledge without first acquiring this mutex.
  // A handler which escapes by throwing still reacquires it during unwinding.
  auto& thread = *GuestThread::current();
  GuestThread::ConditionWaitCleanup cleanup{thread.condition_wait_cleanup,
      [](void* mutex) { (void)mutex_acquire(static_cast<std::uint32_t*>(mutex), -1, false, false); },
      m, thread.cpu().sp() + 8};
  thread.condition_wait_cleanup = &cleanup;
  struct Restore {
    GuestThread& thread; GuestThread::ConditionWaitCleanup& cleanup;
    ~Restore() { thread.condition_wait_cleanup = cleanup.previous; }
  } restore{thread, cleanup};
  r = futex_wait(thread, h2g(c), seq, end);
  auto locked = cleanup.pending ? mutex_acquire(m, -1, false) : 0;
  return locked ? locked : r == -err::etimedout ? err::etimedout : 0;
}
int cond_wait(std::uint32_t* c, std::uint32_t* m) { return cond_clock(c, m, 0, nullptr); }
int cond_timed(std::uint32_t* c, std::uint32_t* m, const Timespec* t) {
  return cond_clock(c, m, (atom(*c).load() & 2) ? 1 : 0, t);
}
int cond_monotonic(std::uint32_t* c, std::uint32_t* m, const Timespec* t) { return cond_clock(c, m, 1, t); }
int rw_init(Rwlock* r, const std::uint32_t* a) {
  if (a && *a) return err::enotsup;
  *r = {}; return 0;
}
int rw_destroy(Rwlock* r) { return atom(r->state).load() ? err::ebusy : 0; }
int rw_acquire(Rwlock* r, bool writer, bool attempt, std::int64_t end = -1) {
  for (;;) {
    auto seq = atom(r->sequence).load(std::memory_order_acquire);
    auto value = atom(r->state).load(std::memory_order_relaxed);
    if (writer ? value == 0 : value != UINT_MAX) {
      if (value == UINT_MAX - 1) return err::eagain;
      if (atom(r->state).compare_exchange_weak(value, writer ? UINT_MAX : value + 1, std::memory_order_acquire)) {
        if (writer) atom(r->owner).store(owner());
        return 0;
      }
      continue;
    }
    if (attempt) return err::ebusy;
    if (value == UINT_MAX && atom(r->owner).load() == owner()) return err::edeadlk;
    if (wait(r->sequence, seq, end) == -err::etimedout) return err::etimedout;
  }
}
int rw_read(Rwlock* r) { return rw_acquire(r, false, false); }
int rw_write(Rwlock* r) { return rw_acquire(r, true, false); }
int rw_tryread(Rwlock* r) { return rw_acquire(r, false, true); }
int rw_trywrite(Rwlock* r) { return rw_acquire(r, true, true); }
int rw_timedread(Rwlock* r, const Timespec* t) {
  std::int64_t end; int e = deadline32(t, 0, &end); return e ? e : rw_acquire(r, false, false, end);
}
int rw_timedwrite(Rwlock* r, const Timespec* t) {
  std::int64_t end; int e = deadline32(t, 0, &end); return e ? e : rw_acquire(r, true, false, end);
}
int rw_unlock(Rwlock* r) {
  auto value = atom(r->state).load();
  if (value == UINT_MAX) {
    if (atom(r->owner).load() != owner()) return err::eperm;
    atom(r->owner).store(0); atom(r->state).store(0, std::memory_order_release);
  } else do { if (!value) return err::eperm; }
    while (!atom(r->state).compare_exchange_weak(value, value - 1, std::memory_order_release));
  atom(r->sequence).fetch_add(1, std::memory_order_release); futex_wake(h2g(&r->sequence), INT_MAX, sync_census::WakeSource::rwlock); return 0;
}
int attr_init(Attr* a) { *a = {0, 0, 1024 * 1024, 4096, 0, 0}; return 0; }
int attr_destroy(Attr*) { return 0; }
int attr_setstack(Attr* a, GuestAddr base, std::uint32_t size) {
  if (size < 16384) return err::einval;
  a->stack = static_cast<std::uint32_t>(base); a->size = size; return 0;
}
int attr_getstack(const Attr* a, GuestAddr* base, std::uint64_t* size) { *base = a->stack; *size = a->size; return 0; }
int attr_setsize(Attr* a, std::uint32_t v) { if (v < 16384) return err::einval; a->size = v; return 0; }
int attr_getsize(const Attr* a, std::uint64_t* v) { *v = a->size; return 0; }
int attr_setguard(Attr* a, std::uint32_t v) { a->guard = v; return 0; }
int attr_getguard(const Attr* a, std::uint64_t* v) { *v = a->guard; return 0; }
int attr_setdetach(Attr* a, int v) { if (v < 0 || v > 1) return err::einval; a->flags = (a->flags & ~1u) | v; return 0; }
int attr_getdetach(const Attr* a, int* v) { *v = a->flags & 1; return 0; }
int attr_setpolicy(Attr* a, int v) { if (v < 0 || v > 2) return err::einval; a->policy = v; return 0; }
int attr_getpolicy(const Attr* a, int* v) { *v = a->policy; return 0; }
int attr_setparam(Attr* a, const int* v) { a->priority = *v; return 0; }
int attr_getparam(const Attr* a, int* v) { *v = a->priority; return 0; }
int attr_setinherit(Attr* a, int v) { if (v < 0 || v > 1) return err::einval; a->flags = (a->flags & ~12u) | (v ? 4u : 8u); return 0; }
int attr_getinherit(const Attr* a, int* v) { *v = (a->flags & 4u) ? 1 : (a->flags & 8u) ? 0 : a->policy == 0; return 0; }
int create(std::uint32_t* handle, const Attr* a, GuestAddr fn, GuestAddr arg) {
  if (!handle) return err::einval;
  threads::Options o;
  if (a) {
    o.stack = a->stack; o.stack_size = a->size; o.guard_size = a->guard; o.detached = a->flags & 1;
    // Match Bionic's historical SCHED_OTHER behavior and explicit overrides.
    const bool apply_schedule = !(a->flags & 4u) && ((a->flags & 8u) || a->policy);
    if (apply_schedule) {
      if (!a->policy && a->priority) return err::einval;
      if (a->policy || a->priority) return err::enotsup;
    }
  }
  o.on_exit = run_key_destructors;
  GuestAddr wide = 0;
  int r = -threads::create(o, fn, arg, &wide);
  if (!r) *handle = static_cast<std::uint32_t>(wide);
  return r;
}
int getattr(GuestAddr h, Attr* a) {
  threads::Options o; int r = threads::describe(h, &o, nullptr, nullptr); if (r) return -r;
  *a = {std::uint32_t(o.detached), std::uint32_t(o.stack), std::uint32_t(o.stack_size), std::uint32_t(o.guard_size), 0, 0}; return 0;
}
// ARM sem_t is one word (bionic semaphore.h); futexes use that exact word.
int sem_init(std::uint32_t* s, int shared, unsigned count) {
  if (shared) return failure(err::enotsup);
  if (count > INT_MAX) return failure(err::einval);
  *s = count; return 0;
}
int sem_destroy(std::uint32_t*) { return 0; }
int sem_value(std::uint32_t* s, int* v) { *v = static_cast<int>(atom(*s).load()); return 0; }
int sem_post(std::uint32_t* s) {
  auto value = atom(*s).load();
  do { if (value == INT_MAX) return failure(err::eoverflow); }
    while (!atom(*s).compare_exchange_weak(value, value + 1, std::memory_order_release));
  futex_wake(h2g(s), 1, sync_census::WakeSource::semaphore); return 0;
}
int sem_acquire(std::uint32_t* s, bool attempt, std::int64_t end = -1) {
  for (;;) {
    auto value = atom(*s).load(std::memory_order_relaxed);
    if (value) {
      if (atom(*s).compare_exchange_weak(value, value - 1, std::memory_order_acquire)) return 0;
      continue;
    }
    if (attempt) return failure(err::eagain);
    int r = wait(*s, 0, end);
    if (r == -err::eintr || r == -err::etimedout) return static_cast<int>(result(r));
  }
}
int sem_wait(std::uint32_t* s) { return sem_acquire(s, false); }
int sem_try(std::uint32_t* s) { return sem_acquire(s, true); }
int sem_clock(std::uint32_t* s, int c, const Timespec* t) {
  std::int64_t end; int r = deadline32(t, c, &end); return r ? failure(r) : sem_acquire(s, false, end);
}
int sem_timed(std::uint32_t* s, const Timespec* t) { return sem_clock(s, 0, t); }
// Cond/rwlock attrs use bit 0 for shared; mutex attrs use bit 4.
int cond_shared_set(std::uint32_t* a, int v) { if (v < 0 || v > 1) return err::einval; *a = (*a & ~1u) | v; return 0; }
int cond_shared_get(const std::uint32_t* a, int* v) { *v = *a & 1; return 0; }
int small_init(std::uint32_t* a) { *a = 0; return 0; }
int small_destroy(std::uint32_t*) { return 0; }
int mutex_type_set(std::uint32_t* a, int v) {
  if (v < 0 || v > 2) return err::einval;
  *a = (*a & ~3u) | v; return 0;
}
int mutex_shared_set(std::uint32_t* a, int v) {
  if (v < 0 || v > 1) return err::einval;
  *a = (*a & ~0x10u) | (v << 4); return 0;
}
int cond_clock_set(std::uint32_t* a, int v) {
  if (v != 0 && v != 1) return err::einval;
  *a = (*a & ~2u) | (v << 1); return 0;
}
}
bool try_mutex32(CallContext& ctx, bool unlock) {
  auto* m = static_cast<std::uint32_t*>(g2h(static_cast<std::uint32_t>(ctx.x(0))));
  if (!m) return false;
  auto state = atom(*m);
  auto value = state.load(std::memory_order_relaxed);
  const auto type = value & type_mask;
  const auto me = std::uint32_t(ctx.thread().mutex_owner_index()) << 16;
  if (value == 0xffff || type == type_mask || (type && (value & 0x2000))) return false;
  std::uint32_t next;
  auto order = std::memory_order_relaxed;
  if (unlock) {
    if (!(value & 3) || (type && (value & 0xffff0000) != me)) return false;
    if (type == 0x4000 && (value & count_mask)) {
      // Only the owner changes recursion depth; keep a waiter's state bits.
      next = value - 4;
    } else {
      // Final contended release must leave the JIT before its futex wake.
      if ((value & 3) != 1) return false;
      next = value & attr_mask;
      order = std::memory_order_release;
    }
  } else {
    if (value & 0x2000) return false;
    if (!(value & 3)) {
      next = (value & attr_mask) | me | 1u;
      order = std::memory_order_acquire;
    } else {
      if (type != 0x4000 || (value & 0xffff0000) != me ||
          (value & count_mask) == count_mask) return false;
      next = value + 4;
    }
  }
  // A waiter may promote 1 to 2 concurrently. Decline without consuming the
  // call on a failed CAS; the existing handler retains all waits and errors.
  if (!state.compare_exchange_strong(value, next, order)) return false;
  ctx.return_x(0);
  return true;
}
void register_threads32_adapters() {
#define A(n, f) abi32::adapt<&f>(n)
  A("pthread_create", create); A("pthread_getattr_np", getattr);
  A("pthread_mutex_init", mutex_init); A("pthread_mutex_destroy", mutex_destroy);
  A("pthread_mutex_lock", mutex_lock); A("pthread_mutex_trylock", mutex_try); A("pthread_mutex_unlock", mutex_unlock);
  A("pthread_mutex_timedlock", mutex_timed); A("pthread_mutex_clocklock", mutex_clock);
  A("pthread_cond_init", cond_init); A("pthread_cond_destroy", cond_destroy); A("pthread_cond_wait", cond_wait);
  A("pthread_cond_timedwait", cond_timed); A("pthread_cond_timedwait_monotonic_np", cond_monotonic);
  A("pthread_cond_clockwait", cond_clock); A("pthread_cond_signal", cond_signal); A("pthread_cond_broadcast", cond_broadcast);
  A("pthread_rwlock_init", rw_init); A("pthread_rwlock_destroy", rw_destroy); A("pthread_rwlock_unlock", rw_unlock);
  A("pthread_rwlock_rdlock", rw_read); A("pthread_rwlock_wrlock", rw_write); A("pthread_rwlock_tryrdlock", rw_tryread);
  A("pthread_rwlock_trywrlock", rw_trywrite); A("pthread_rwlock_timedrdlock", rw_timedread); A("pthread_rwlock_timedwrlock", rw_timedwrite);
  A("pthread_attr_init", attr_init); A("pthread_attr_destroy", attr_destroy);
  A("pthread_attr_setstack", attr_setstack); A("pthread_attr_getstack", attr_getstack);
  A("pthread_attr_setstacksize", attr_setsize); A("pthread_attr_getstacksize", attr_getsize);
  A("pthread_attr_setguardsize", attr_setguard); A("pthread_attr_getguardsize", attr_getguard);
  A("pthread_attr_setdetachstate", attr_setdetach); A("pthread_attr_getdetachstate", attr_getdetach);
  A("pthread_attr_setschedpolicy", attr_setpolicy); A("pthread_attr_getschedpolicy", attr_getpolicy);
  A("pthread_attr_setschedparam", attr_setparam); A("pthread_attr_getschedparam", attr_getparam);
  A("pthread_attr_setinheritsched", attr_setinherit); A("pthread_attr_getinheritsched", attr_getinherit);
  A("sem_init", sem_init); A("sem_destroy", sem_destroy); A("sem_getvalue", sem_value); A("sem_post", sem_post);
  A("sem_wait", sem_wait); A("sem_trywait", sem_try); A("sem_timedwait", sem_timed); A("sem_clockwait", sem_clock);
  A("pthread_condattr_setpshared", cond_shared_set); A("pthread_condattr_getpshared", cond_shared_get);
  A("pthread_rwlockattr_setpshared", cond_shared_set); A("pthread_rwlockattr_getpshared", cond_shared_get);
  A("pthread_mutexattr_init", small_init); A("pthread_mutexattr_destroy", small_destroy);
  A("pthread_mutexattr_settype", mutex_type_set); A("pthread_mutexattr_setpshared", mutex_shared_set);
  A("pthread_condattr_init", small_init); A("pthread_condattr_destroy", small_destroy);
  A("pthread_condattr_setclock", cond_clock_set);
  A("pthread_rwlockattr_init", small_init); A("pthread_rwlockattr_destroy", small_destroy);
#undef A
}
}
