#include "vfs.h"
#include "abi32.h"
#include "core/signals.h"
#include "common/log.h"
#include <bit>
#include <cstring>

namespace venture::hle::libc {
namespace {
// bionic libc/include/bits/signal_types.h: LP64 FLAGS FIRST. The handler,
// mask, flags, restorer order is the ILP32 ABI, not AArch64.
struct Sigaction {
  std::uint32_t flags, pad;
  GuestAddr handler;
  std::uint64_t mask;
  GuestAddr restorer;
};
struct Stack {
  GuestAddr sp;
  std::int32_t flags, pad;
  std::uint64_t size;
};
static_assert(sizeof(Sigaction) == 32 && offsetof(Sigaction, handler) == 8 &&
              offsetof(Sigaction, mask) == 16);
static_assert(sizeof(Stack) == 24 && offsetof(Stack, size) == 16);
int action(int n, const Sigaction* a, Sigaction* old) {
  signals::Action next{}, prev{};
  if (a) next = {a->handler, a->mask, a->flags};
  int r = signals::set_action(n, a ? &next : nullptr, old ? &prev : nullptr);
  if (!r && old) *old = {prev.flags, 0, prev.handler, prev.mask, 0};
  return static_cast<int>(result(r));
}
GuestAddr signal_fn(int n, GuestAddr handler) {
  Sigaction a{0x10000000, 0, handler, 0, 0}, old{};
  return action(n, &a, &old) < 0 ? UINT64_MAX : old.handler;
}
int mask(int how, const std::uint64_t* set, std::uint64_t* old) {
  return static_cast<int>(result(signals::set_mask(*GuestThread::current(), how, set, old)));
}
int pthread_mask(int how, const std::uint64_t* set, std::uint64_t* old) {
  return -signals::set_mask(*GuestThread::current(), how, set, old);
}
int empty(std::uint64_t* set) {
  *set = 0;
  return 0;
}
int fill(std::uint64_t* set) {
  *set = UINT64_MAX;
  return 0;
}
int addset(std::uint64_t* set, int n) {
  if (n < 1 || n > 64) return failure(err::einval);
  *set |= 1ull << (n - 1);
  return 0;
}
int delset(std::uint64_t* set, int n) {
  if (n < 1 || n > 64) return failure(err::einval);
  *set &= ~(1ull << (n - 1));
  return 0;
}
int member(const std::uint64_t* set, int n) {
  if (n < 1 || n > 64) return failure(err::einval);
  return (*set >> (n - 1)) & 1;
}
int pending(std::uint64_t* set) {
  *set = GuestThread::current()->pending_signals.load();
  return 0;
}
int raise_fn(int n) {
  return static_cast<int>(result(signals::send_to_thread(
      GuestThread::current()->tid(), {.signo = n, .sender_pid = 1000, .sender_uid = 10125})));
}
int kill_fn(int pid, int n) {
  if (pid != 0 && pid != -1 && pid != 1000) return failure(err::esrch);
  return static_cast<int>(
      result(signals::send_to_process({.signo = n, .sender_pid = 1000, .sender_uid = 10125})));
}
int tgkill_fn(int pid, int tid, int n) {
  if (pid != 1000) return failure(err::esrch);
  return static_cast<int>(
      result(signals::send_to_thread(tid, {.signo = n, .sender_pid = 1000, .sender_uid = 10125})));
}
int tkill_fn(int tid, int n) { return tgkill_fn(1000, tid, n); }
int sigqueue_fn(int pid, int n, std::uint64_t value) {
  if (pid != 1000) return failure(err::esrch);
  return static_cast<int>(result(signals::send_to_process(
      {.signo = n, .code = -1, .sender_pid = 1000, .sender_uid = 10125, .value = value})));
}
int altstack(const Stack* a, Stack* old) {
  signals::AltStack next{}, prev{};
  if (a) next = {a->sp, a->size, a->flags};
  int r =
      signals::set_alt_stack(*GuestThread::current(), a ? &next : nullptr, old ? &prev : nullptr);
  if (!r && old) *old = {prev.base, prev.flags, 0, prev.size};
  return static_cast<int>(result(r));
}
int suspend(const std::uint64_t* set) {
  auto& t = *GuestThread::current();
  std::uint64_t old;
  int r = signals::set_mask(t, 2, set, &old);
  if (r) return result(r);
  while (!signals::deliver_pending(t)) {
    if (sleep_until(t, monotonic_ns() + 1000000000) == -err::eintr) break;
  }
  signals::set_mask(t, 2, &old, nullptr);
  return failure(err::eintr);
}
// bionic arm64 setjmp.S: 32 64-bit words; pairs are saved in descending
// register order. We preserve the ABI offsets, use cookie zero and checksum.
int sigsetjmp_fn(CallContext& ctx, std::uint64_t* b, int save) {
  auto& c = ctx.cpu();
  std::memset(b, 0, 256);
  b[0] = save ? 1 : 0;
  b[1] = save ? ctx.thread().blocked_signals : 0;
  b[2] = c.lr();
  b[3] = c.sp();
  for (int i = 0; i < 6; ++i) {
    b[4 + i * 2] = c.x(28 - i * 2);
    b[5 + i * 2] = c.x(29 - i * 2);
  }
  for (int i = 0; i < 4; ++i) {
    b[16 + i * 2] = c.v(14 - i * 2).lo;
    b[17 + i * 2] = c.v(15 - i * 2).lo;
  }
  for (int i = 0; i < 24; ++i) b[24] ^= b[i];
  return 0;
}
int setjmp_fn(CallContext& c, std::uint64_t* b) { return sigsetjmp_fn(c, b, 1); }
int underscore_setjmp(CallContext& c, std::uint64_t* b) { return sigsetjmp_fn(c, b, 0); }
void longjmp_fn(CallContext& ctx, const std::uint64_t* b, int value) {
  std::uint64_t sum = 0;
  for (int i = 0; i < 24; ++i) sum ^= b[i];
  if (sum != b[24]) {
    log::error("libc", "invalid guest jmp_buf checksum");
    signals::deliver_synchronous(ctx.thread(), {.signo = signals::kSigAbrt});
    return;
  }
  if (b[0] & 1) signals::set_mask(ctx.thread(), 2, &b[1], nullptr);
  auto& c = ctx.cpu();
  c.set_lr(b[2]);
  c.set_sp(b[3]);
  for (int i = 0; i < 6; ++i) {
    c.set_x(28 - i * 2, b[4 + i * 2]);
    c.set_x(29 - i * 2, b[5 + i * 2]);
  }
  for (int i = 0; i < 4; ++i) {
    c.set_v(14 - i * 2, {b[16 + i * 2], 0});
    c.set_v(15 - i * 2, {b[17 + i * 2], 0});
  }
  c.set_x(0, static_cast<std::uint32_t>(value ? value : 1));
  c.set_pc(b[2]);
  c.clear_exclusive();
  // A void handler has no result writeback in hle::wrap: PC/X0 survive.
}
void psiginfo_fn(const void* info, const char* text) {
  auto n = *static_cast<const int*>(info);
  log::warn("guest", "{}: signal {}", text ? text : "", n);
}
int timedwait(const std::uint64_t* set, void* out, const Timespec* timeout) {
  if (!set) return failure(err::efault);
  std::int64_t end = -1;
  if (timeout) {
    if (timeout->sec < 0) return failure(err::einval);
    int r = deadline(timeout, 1, &end);
    if (r) return result(r);
    auto now = monotonic_ns();
    end = end > INT64_MAX - now ? INT64_MAX : now + end;
  }
  signals::Info info;
  int r = signals::wait_signal(*GuestThread::current(), *set, &info, end);
  if (r > 0 && out) {
    std::memset(out, 0, 128);
    auto* bytes = static_cast<unsigned char*>(out);
    std::memcpy(bytes, &info.signo, 4);
    std::memcpy(bytes + 8, &info.code, 4);
    std::memcpy(bytes + 16, &info.sender_pid, 4);
    std::memcpy(bytes + 20, &info.sender_uid, 4);
    std::memcpy(bytes + 24, &info.value, 8);
  }
  return result(r);
}
int waitinfo(const std::uint64_t* set, void* info) { return timedwait(set, info, nullptr); }
int sigwait_fn(const std::uint64_t* set, int* out) {
  if (!out) return err::einval;
  auto& t = *GuestThread::current();
  int old = *t.errno_location();
  int r = timedwait(set, nullptr, nullptr);
  int e = *t.errno_location();
  t.set_errno(old);
  if (r < 0) return e;
  *out = r;
  return 0;
}
int pause_fn() {
  auto mask = GuestThread::current()->blocked_signals;
  return suspend(&mask);
}
// bionic bits/signal_types.h: sigaction64 has a different field order,
// and sigset64_t is two ARM unsigned longs (alignment 4, not uint64_t).
struct Sigaction32 { std::uint32_t handler, mask, flags, restorer; };
struct Sigaction64_32 { std::uint32_t handler, flags, restorer, mask[2]; };
struct Stack32 { std::uint32_t sp; std::int32_t flags; std::uint32_t size; };
static_assert(sizeof(Sigaction32) == 16 && sizeof(Sigaction64_32) == 20 && sizeof(Stack32) == 12);
int action32(int n, const Sigaction32* a, Sigaction32* old) {
  signals::Action next{}, prev{};
  if (a) next = {a->handler, a->mask, a->flags};
  int r = signals::set_action(n, a ? &next : nullptr, old ? &prev : nullptr);
  if (!r && old) *old = {std::uint32_t(prev.handler), std::uint32_t(prev.mask), prev.flags, 0};
  return static_cast<int>(result(r));
}
int action64_32(int n, const Sigaction64_32* a, Sigaction64_32* old) {
  signals::Action next{}, prev{};
  if (a) next = {a->handler, a->mask[0] | (std::uint64_t(a->mask[1]) << 32), a->flags};
  int r = signals::set_action(n, a ? &next : nullptr, old ? &prev : nullptr);
  if (!r && old) *old = {std::uint32_t(prev.handler), prev.flags, 0, {std::uint32_t(prev.mask), std::uint32_t(prev.mask >> 32)}};
  return static_cast<int>(result(r));
}
std::uint32_t signal32(int n, GuestAddr handler) { return static_cast<std::uint32_t>(signal_fn(n, handler)); }
int mask32_impl(int how, const std::uint32_t* set, std::uint32_t* old) {
  std::uint64_t next = set ? *set : 0, prev = 0;
  int r = signals::set_mask(*GuestThread::current(), how, set ? &next : nullptr, old ? &prev : nullptr);
  if (!r && old) *old = static_cast<std::uint32_t>(prev);
  return r;
}
int mask32(int how, const std::uint32_t* set, std::uint32_t* old) { return static_cast<int>(result(mask32_impl(how, set, old))); }
int pthread_mask32(int how, const std::uint32_t* set, std::uint32_t* old) { return -mask32_impl(how, set, old); }
int empty32(std::uint32_t* p) { *p = 0; return 0; }
int fill32(std::uint32_t* p) { *p = UINT32_MAX; return 0; }
int addset32(std::uint32_t* p, int n) { if (n < 1 || n > 32) return failure(err::einval); *p |= 1u << (n - 1); return 0; }
int delset32(std::uint32_t* p, int n) { if (n < 1 || n > 32) return failure(err::einval); *p &= ~(1u << (n - 1)); return 0; }
int member32(const std::uint32_t* p, int n) { if (n < 1 || n > 32) return failure(err::einval); return (*p >> (n - 1)) & 1; }
int pending32(std::uint32_t* p) { *p = static_cast<std::uint32_t>(GuestThread::current()->pending_signals.load()); return 0; }
int suspend32(const std::uint32_t* p) { std::uint64_t set = *p; return suspend(&set); }
int altstack32(const Stack32* a, Stack32* old) {
  signals::AltStack next{}, prev{};
  if (a) next = {a->sp, a->size, a->flags};
  auto r = signals::set_alt_stack(*GuestThread::current(), a ? &next : nullptr, old ? &prev : nullptr);
  if (!r && old) *old = {std::uint32_t(prev.base), prev.flags, std::uint32_t(prev.size)};
  return static_cast<int>(result(r));
}
// bionic arch-arm/bionic/setjmp.S: words 4..19 d8..d15, 20 FPSCR,
// 21..28 r4..r11, 29 LR, 30 SP, 31 checksum. jmp_buf is 256 bytes;
// sigjmp_buf is 260 but bionic uses the same first 256 bytes for both.
int sigsetjmp32(CallContext& ctx, std::uint32_t* b, int save) {
  auto& c = ctx.cpu();
  std::memset(b, 0, 256);
  b[0] = save != 0;
  if (save) { b[1] = std::uint32_t(ctx.thread().blocked_signals); b[2] = std::uint32_t(ctx.thread().blocked_signals >> 32); }
  for (int i = 0; i < 8; ++i) { auto d = c.v(i + 8).lo; std::memcpy(b + 4 + 2 * i, &d, 8); b[21 + i] = std::uint32_t(c.x(i + 4)); }
  b[20] = c.fpcr(); b[29] = std::uint32_t(c.lr()); b[30] = std::uint32_t(c.sp());
  // The bionic checksum macro covers words 0..28 (LR/SP excluded).
  for (int i = 0; i < 29; ++i) b[31] ^= b[i];
  return 0;
}
int setjmp32(CallContext& c, std::uint32_t* b) { return sigsetjmp32(c, b, 1); }
int underscore_setjmp32(CallContext& c, std::uint32_t* b) { return sigsetjmp32(c, b, 0); }
void longjmp32(CallContext& ctx, const std::uint32_t* b, int value) {
  std::uint32_t sum = 0;
  for (int i = 0; i < 29; ++i) sum ^= b[i];
  if (sum != b[31]) { log::error("libc", "invalid A32 jmp_buf checksum"); signals::deliver_synchronous(ctx.thread(), {.signo = signals::kSigAbrt}); return; }
  if (b[0] & 1) { std::uint64_t mask = b[1] | (std::uint64_t(b[2]) << 32); signals::set_mask(ctx.thread(), 2, &mask, nullptr); }
  auto& c = ctx.cpu();
  for (int i = 0; i < 8; ++i) { std::uint64_t d; std::memcpy(&d, b + 4 + 2 * i, 8); c.set_v(i + 8, {d, 0}); c.set_x(i + 4, b[21 + i]); }
  c.set_fpcr(b[20]); c.set_lr(b[29]); c.set_sp(b[30]); c.set_pc(b[29]);
  c.set_x(0, std::uint32_t(value ? value : 1)); c.clear_exclusive();
}
}  // namespace
void register_signals(HleLibrary& lib) {
  if (process_config().isa() == GuestIsa::a32) {
#define A(n, f) abi32::adapt<&f>(n)
    A("sigaction", action32); A("sigaction64", action64_32); A("signal", signal32); A("bsd_signal", signal32);
    A("sigprocmask", mask32); A("pthread_sigmask", pthread_mask32);
    A("sigprocmask64", mask); A("pthread_sigmask64", pthread_mask);
    A("sigemptyset", empty32); A("sigfillset", fill32); A("sigaddset", addset32); A("sigdelset", delset32); A("sigismember", member32);
    A("sigemptyset64", empty); A("sigfillset64", fill); A("sigaddset64", addset); A("sigdelset64", delset); A("sigismember64", member);
    A("sigpending", pending32); A("sigpending64", pending); A("sigsuspend", suspend32); A("sigsuspend64", suspend);
    A("sigaltstack", altstack32);
    A("setjmp", setjmp32); A("_setjmp", underscore_setjmp32); A("sigsetjmp", sigsetjmp32); A("__sigsetjmp", sigsetjmp32);
    A("longjmp", longjmp32); A("_longjmp", longjmp32); A("siglongjmp", longjmp32);
#undef A
  }
#define B(n, f) VENTURE_LIBC_BIND(lib, n, f)
  B("sigaction", action);
  B("sigaction64", action);
  B("signal", signal_fn);
  B("bsd_signal", signal_fn);
  B("sigprocmask", mask);
  B("sigprocmask64", mask);
  B("pthread_sigmask", pthread_mask);
  B("pthread_sigmask64", pthread_mask);
  B("sigemptyset", empty);
  B("sigemptyset64", empty);
  B("sigfillset", fill);
  B("sigfillset64", fill);
  B("sigaddset", addset);
  B("sigaddset64", addset);
  B("sigdelset", delset);
  B("sigdelset64", delset);
  B("sigismember", member);
  B("sigismember64", member);
  B("sigpending", pending);
  B("sigpending64", pending);
  B("raise", raise_fn);
  B("kill", kill_fn);
  B("tgkill", tgkill_fn);
  B("tkill", tkill_fn);
  B("sigqueue", sigqueue_fn);
  B("sigaltstack", altstack);
  B("sigsuspend", suspend);
  B("sigsuspend64", suspend);
  B("psiginfo", psiginfo_fn);
  B("setjmp", setjmp_fn);
  B("_setjmp", underscore_setjmp);
  B("sigsetjmp", sigsetjmp_fn);
  B("longjmp", longjmp_fn);
  B("_longjmp", longjmp_fn);
  B("siglongjmp", longjmp_fn);
  B("sigwait", sigwait_fn);
  B("sigwait64", sigwait_fn);
  B("sigtimedwait", timedwait);
  B("sigtimedwait64", timedwait);
  B("sigwaitinfo", waitinfo);
  B("sigwaitinfo64", waitinfo);
  B("pause", pause_fn);
#undef B
}
}  // namespace venture::hle::libc
