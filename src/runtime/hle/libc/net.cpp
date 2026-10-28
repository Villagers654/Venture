#include "vfs_net.h"
#include "abi32.h"
#include "libc_a.h"
#include "core/signals.h"
#include "common/environment.h"
#include "common/log.h"
#include <algorithm>
#include <bit>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <thread>

namespace venture::hle::libc {
namespace {
int pause_io(sync_census::IoScope& trace) {
  const auto begin = sync_census::enabled ? monotonic_ns() : 0;
  const auto status = sleep_until(*GuestThread::current(), monotonic_ns() + 1000000);
  if (sync_census::enabled) trace.slept(begin, monotonic_ns());
  return status;
}
// Resolver workers own all buffers. An interrupted guest can return without
// waiting for the native resolver or letting it write into expired guest memory.
template <class Job, class Work>
int resolve_async(std::shared_ptr<Job> job, Work work) {
  sync_census::IoScope trace(sync_census::IoSource::resolver, (sync_census::enabled && GuestThread::current() ? GuestThread::current()->tid() : 0));
  try {
    std::thread([job, work = std::move(work)] {
      try {
        job->result = work(*job);
      } catch (...) {
        job->result = 6;
      }
      job->done.store(true, std::memory_order_release);
    }).detach();
  } catch (...) {
    failure(err::eagain);
    return 11;
  }
  while (!job->done.load(std::memory_order_acquire)) {
    int r = pause_io(trace);
    if (r) {
      result(r);
      return 11;
    }
  }
  return job->result;
}
int lookup_interruptible(const char* node, const char* service, int family, int type, int protocol,
                         int flags, std::vector<host::Lookup>& out) {
  struct Job {
    std::atomic<bool> done{false};
    int result = 0;
    std::vector<host::Lookup> entries;
  };
  auto job = std::make_shared<Job>();
  int r =
      resolve_async(job, [name = std::string(node ? node : ""),
                          port = std::string(service ? service : ""), has_node = node != nullptr,
                          has_service = service != nullptr, family, type, protocol, flags](Job& j) {
        return host::lookup(has_node ? name.c_str() : nullptr, has_service ? port.c_str() : nullptr,
                            family, type, protocol, flags, j.entries);
      });
  if (!r) out = std::move(job->entries);
  return r;
}
struct PipeState {
  std::mutex mutex;
  std::deque<unsigned char> bytes;
  bool reader = true, writer = true;
  std::atomic<std::uint64_t> read_epoch{0}, write_epoch{0};
  std::int64_t read_ready_ns = -1, write_ready_ns = -1;
  ReadinessSource readiness;
};
class PipeFile final : public File {
 public:
  bool epoll_supported() const override { return true; }
  PipeFile(std::shared_ptr<PipeState> state, bool write)
      : state_(std::move(state)), writer_(write) {
    flags = write ? 1 : 0;
  }
  ~PipeFile() override {
    std::lock_guard lock(state_->mutex);
    (writer_ ? state_->writer : state_->reader) = false;
    state_->readiness.notify(poll_in | poll_out | poll_err | poll_hup);
  }
  int watch(ReadinessWaitSet& wait, short events) override { wait.add(state_->readiness, events); return 0; }
  std::int64_t read(void* p, std::uint64_t n) override { return read_impl(p, n, false); }
  std::int64_t read_impl(void* p, std::uint64_t n, bool peek) {
    if (writer_) return -err::ebadf;
    if (!n) return 0;
    sync_census::IoScope trace(sync_census::IoSource::pipe_read, (sync_census::enabled && GuestThread::current() ? GuestThread::current()->tid() : 0));
    for (;;) {
      {
        std::lock_guard lock(state_->mutex);
        if (!state_->reader) return 0;
        if (!state_->bytes.empty()) {
          if (sync_census::enabled) trace.readiness(state_->read_ready_ns);
          n = std::min<std::uint64_t>(n, state_->bytes.size());
          if (peek) {
            for (std::uint64_t i = 0; i < n; ++i)
              static_cast<unsigned char*>(p)[i] = state_->bytes[i];
            return n;
          }
          for (std::uint64_t i = 0; i < n; ++i) {
            static_cast<unsigned char*>(p)[i] = state_->bytes.front();
            state_->bytes.pop_front();
          }
          ++state_->write_epoch;
          if (sync_census::enabled) state_->write_ready_ns = monotonic_ns();
          if (state_->bytes.empty()) ++state_->read_epoch;
          state_->readiness.notify(poll_out);
          return n;
        }
        if (!state_->writer) return 0;
      }
      if (flags.load() & o_nonblock) return -err::eagain;
      const auto begin = sync_census::enabled ? monotonic_ns() : 0;
      int r = wait_ready(poll_in, INT64_MAX);
      if (sync_census::enabled) trace.slept(begin, monotonic_ns());
      if (r) return r;
    }
  }
  std::int64_t write(const void* p, std::uint64_t n) override { return write_impl(p, n, true); }
  std::int64_t write_impl(const void* p, std::uint64_t n, bool sigpipe) {
    if (!writer_) return -err::ebadf;
    if (!n) return 0;
    sync_census::IoScope trace(sync_census::IoSource::pipe_write, (sync_census::enabled && GuestThread::current() ? GuestThread::current()->tid() : 0));
    ReadinessWaitSet wait;
    bool subscribed = false;
    for (;;) {
      const auto sequence = wait.sequence();
      bool broken = false;
      {
        std::lock_guard lock(state_->mutex);
        if (!state_->reader || !state_->writer)
          broken = true;
        else {
          auto room = 65536 - state_->bytes.size();
          if (room && (n > 4096 || room >= n)) {
            if (sync_census::enabled) trace.readiness(state_->write_ready_ns);
            auto count = std::min<std::uint64_t>(room, n);
            auto* b = static_cast<const unsigned char*>(p);
            if (sync_census::enabled && state_->bytes.empty()) state_->read_ready_ns = monotonic_ns();
            state_->bytes.insert(state_->bytes.end(), b, b + count);
            ++state_->read_epoch;
            state_->readiness.notify(poll_in);
            return count;
          }
        }
      }
      if (broken) {
        if (sigpipe) signals::send_to_thread(GuestThread::current()->tid(), {.signo = 13});
        return -err::epipe;
      }
      if (flags.load() & o_nonblock) return -err::eagain;
      if (!subscribed) {
        wait.add(state_->readiness, poll_out);
        subscribed = true;
        continue; // Recheck the complete atomic-write predicate after registration.
      }
      const auto begin = sync_census::enabled ? monotonic_ns() : 0;
      int r = wait.wait(*GuestThread::current(), sequence, INT64_MAX);
      if (sync_census::enabled) trace.slept(begin, monotonic_ns());
      if (r) return r;
    }
  }
  short ready(short e) override {
    std::lock_guard lock(state_->mutex);
    if (writer_)
      return (!state_->reader ? poll_err : 0) | ((state_->bytes.size() < 65536) ? e & poll_out : 0);
    return (!state_->writer ? poll_hup : 0) | (!state_->bytes.empty() ? e & poll_in : 0);
  }
  std::int64_t readiness_time(short events) override {
    std::lock_guard lock(state_->mutex);
    return events & poll_in ? state_->read_ready_ns : events & poll_out ? state_->write_ready_ns : -1;
  }
  std::int64_t available() override {
    std::lock_guard lock(state_->mutex);
    return state_->bytes.size();
  }
  std::uint64_t read_generation() const override { return state_->read_epoch.load(); }
  std::uint64_t write_generation() const override { return state_->write_epoch.load(); }
  void end() {
    std::lock_guard lock(state_->mutex);
    (writer_ ? state_->writer : state_->reader) = false;
    ++state_->read_epoch;
    ++state_->write_epoch;
    state_->readiness.notify(poll_in | poll_out | poll_err | poll_hup);
  }
  int stat(Stat* s) override {
    *s = {};
    s->mode = 0010600;
    return 0;
  }

 private:
  std::shared_ptr<PipeState> state_;
  bool writer_;
};
int pipe2_fn(int* fds, int flags) {
  if (flags & ~(o_cloexec | o_nonblock)) return failure(err::einval);
  if (!fds) return failure(err::efault);
  auto state = std::make_shared<PipeState>();
  auto a = std::make_shared<PipeFile>(state, false), b = std::make_shared<PipeFile>(state, true);
  a->flags = flags & o_nonblock;
  b->flags = (flags & o_nonblock) | 1;
  int x = FdTable::get().insert(a, 0, flags & o_cloexec);
  if (x < 0) return result(x);
  int y = FdTable::get().insert(b, 0, flags & o_cloexec);
  if (y < 0) {
    FdTable::get().close(x);
    return result(y);
  }
  fds[0] = x;
  fds[1] = y;
  return 0;
}
int pipe_fn(int* fds) { return pipe2_fn(fds, 0); }
class EventFd final : public File {
 public:
  bool readiness_notifications() const override { return true; }
  bool epoll_supported() const override { return true; }
  EventFd(std::uint64_t value, bool semaphore) : value_(value), semaphore_(semaphore) { flags = 2; }
  std::int64_t read(void* p, std::uint64_t n) override {
    if (n < 8) return -err::einval;
    sync_census::IoScope trace(sync_census::IoSource::eventfd_read, (sync_census::enabled && GuestThread::current() ? GuestThread::current()->tid() : 0));
    for (;;) {
      {
        std::lock_guard lock(mutex);
        if (value_) {
          if (sync_census::enabled) trace.readiness(read_ready_ns);
          auto value = semaphore_ ? 1 : value_;
          value_ -= value;
          ++write_epoch;
          if (sync_census::enabled) write_ready_ns = monotonic_ns();
          if (!value_) ++read_epoch;
          readiness.notify(poll_out);
          std::memcpy(p, &value, 8);
          return 8;
        }
      }
      if (flags.load() & o_nonblock) return -err::eagain;
      const auto begin = sync_census::enabled ? monotonic_ns() : 0;
      int r = wait_ready(poll_in, INT64_MAX);
      if (sync_census::enabled) trace.slept(begin, monotonic_ns());
      if (r) return r;
    }
  }
  std::int64_t write(const void* p, std::uint64_t n) override {
    if (n < 8) return -err::einval;
    std::uint64_t value;
    std::memcpy(&value, p, 8);
    if (value == UINT64_MAX) return -err::einval;
    sync_census::IoScope trace(sync_census::IoSource::eventfd_write, (sync_census::enabled && GuestThread::current() ? GuestThread::current()->tid() : 0));
    ReadinessWaitSet wait;
    bool subscribed = false;
    for (;;) {
      const auto sequence = wait.sequence();
      {
        std::lock_guard lock(mutex);
        if (value < UINT64_MAX - value_) {
          if (sync_census::enabled) {
            trace.readiness(write_ready_ns);
            if (!value_ && value) read_ready_ns = monotonic_ns();
          }
          value_ += value;
          ++read_epoch;
          readiness.notify(poll_in);
          return 8;
        }
      }
      if (flags.load() & o_nonblock) return -err::eagain;
      if (!subscribed) {
        wait.add(readiness, poll_out);
        subscribed = true;
        continue; // POLLOUT alone need not leave room for this entire value.
      }
      const auto begin = sync_census::enabled ? monotonic_ns() : 0;
      int r = wait.wait(*GuestThread::current(), sequence, INT64_MAX);
      if (sync_census::enabled) trace.slept(begin, monotonic_ns());
      if (r) return r;
    }
  }
  short ready(short e) override {
    std::lock_guard lock(mutex);
    return (value_ ? e & poll_in : 0) | (value_ < UINT64_MAX - 1 ? e & poll_out : 0);
  }
  std::int64_t readiness_time(short events) override {
    std::lock_guard lock(mutex);
    return events & poll_in ? read_ready_ns : events & poll_out ? write_ready_ns : -1;
  }

 private:
  std::uint64_t value_;
  bool semaphore_;
};
int eventfd_fn(unsigned init, int flags) {
  if (flags & ~(1 | o_nonblock | o_cloexec)) return failure(err::einval);
  auto f = std::make_shared<EventFd>(init, flags & 1);
  f->flags = 2 | (flags & o_nonblock);
  return result(FdTable::get().insert(f, 0, flags & o_cloexec));
}
int eventfd_read_fn(int fd, std::uint64_t* p) {
  auto r = fd_read(fd, p, 8);
  return r == 8 ? 0 : result(r);
}
int eventfd_write_fn(int fd, std::uint64_t value) {
  auto r = fd_write(fd, &value, 8);
  return r == 8 ? 0 : result(r);
}
int poll_fn(PollFd* f, std::uint64_t n, int timeout) {
  return static_cast<int>(result(fd_poll(f, n, timeout)));
}
int ppoll_fn(PollFd* f, std::uint64_t n, const Timespec* t, const std::uint64_t* set) {
  if (t && (t->sec < 0 || t->nsec < 0 || t->nsec >= 1000000000)) return failure(err::einval);
  auto& thread = *GuestThread::current();
  std::uint64_t old = 0;
  if (set) signals::set_mask(thread, 2, set, &old);
  auto ms =
      t ? (t->sec > INT_MAX / 1000
               ? INT_MAX
               : std::min<std::int64_t>(INT_MAX, t->sec * 1000 + (t->nsec + 999999) / 1000000))
        : -1;
  int r = poll_fn(f, n, static_cast<int>(ms));
  if (set) signals::set_mask(thread, 2, &old, nullptr);
  return r;
}
int poll_chk(PollFd* f, std::uint64_t n, int ms, std::uint64_t size) {
  return n > size / sizeof(PollFd) ? failure(err::eoverflow) : poll_fn(f, n, ms);
}
int select_fn(int n, std::uint64_t* rd, std::uint64_t* wr, std::uint64_t* ex, Timeval* timeout) {
  if (n < 0 || n > 1024 ||
      (timeout && (timeout->sec < 0 || timeout->usec < 0 || timeout->usec >= 1000000)))
    return failure(err::einval);
  std::vector<PollFd> f;
  for (int i = 0; i < n; ++i) {
    auto bit = 1ull << (i % 64);
    short events = 0;
    if (rd && (rd[i / 64] & bit)) events |= poll_in;
    if (wr && (wr[i / 64] & bit)) events |= poll_out;
    if (ex && (ex[i / 64] & bit)) events |= 2;
    if (events) f.push_back({i, events, 0});
  }
  auto ms = timeout ? (timeout->sec > INT_MAX / 1000
                           ? INT_MAX
                           : std::min<std::int64_t>(
                                 INT_MAX, timeout->sec * 1000 + (timeout->usec + 999) / 1000))
                    : -1;
  auto start = monotonic_ns();
  int r = fd_poll(f.data(), f.size(), ms, sync_census::IoSource::select);
  if (r < 0) return result(r);
  for (auto& p : f)
    if (p.revents & poll_nval) return failure(err::ebadf);
  auto bytes = ((n + 63) / 64) * 8;
  if (rd) std::memset(rd, 0, bytes);
  if (wr) std::memset(wr, 0, bytes);
  if (ex) std::memset(ex, 0, bytes);
  int count = 0;
  for (auto& p : f) {
    auto bit = 1ull << (p.fd % 64);
    if (rd && (p.events & poll_in) && (p.revents & (poll_in | poll_hup | poll_err))) {
      rd[p.fd / 64] |= bit;
      ++count;
    }
    if (wr && (p.events & poll_out) && (p.revents & (poll_out | poll_err))) {
      wr[p.fd / 64] |= bit;
      ++count;
    }
    if (ex && (p.revents & 2)) {
      ex[p.fd / 64] |= bit;
      ++count;
    }
  }
  if (timeout) {
    auto left = std::max<std::int64_t>(0, ms * 1000ll - (monotonic_ns() - start) / 1000);
    *timeout = {left / 1000000, left % 1000000};
  }
  return count;
}
int pselect_fn(int n, std::uint64_t* r, std::uint64_t* w, std::uint64_t* e, const Timespec* t,
               const std::uint64_t* set) {
  if (t && (t->sec < 0 || t->nsec < 0 || t->nsec >= 1000000000)) return failure(err::einval);
  Timeval tv{};
  if (t) tv = {t->sec, (t->nsec + 999) / 1000};
  if (tv.usec == 1000000) {
    ++tv.sec;
    tv.usec = 0;
  }
  auto& thread = *GuestThread::current();
  std::uint64_t old;
  if (set) signals::set_mask(thread, 2, set, &old);
  int value = select_fn(n, r, w, e, t ? &tv : nullptr);
  if (set) signals::set_mask(thread, 2, &old, nullptr);
  return value;
}
// bionic bits/epoll_event.h: ARM32 EABI and AArch64 align data to eight
// bytes (16-byte records); only x86-64 uses a packed epoll_event.
struct EpollEvent {
  std::uint32_t events, pad;
  std::uint64_t data;
};
static_assert(sizeof(EpollEvent) == 16);
class Epoll final : public File {
 public:
  bool epoll_supported() const override { return true; }
  int watch(ReadinessWaitSet& wait, short events) override {
    wait.add(readiness, events);
    std::lock_guard lock(mutex);
    for (auto& [key, w] : watches) if (!w.disabled) if (auto file = w.file.lock())
      if (auto status = file->watch(wait, static_cast<short>(w.event.events))) return status;
    return 0;
  }
  struct Watch {
    std::weak_ptr<File> file;
    EpollEvent event;
    std::uint32_t last = 0;
    bool disabled = false;
    std::uint64_t read_generation = 0, write_generation = 0;
  };
  using Key = std::pair<int, std::uintptr_t>;
  std::map<Key, Watch> watches;
  std::atomic<std::uint64_t> changes{0};
  Key cursor{-1, 0};
  static std::uint32_t pending(Watch& w, File& f, bool consume) {
    auto current = static_cast<std::uint32_t>(f.ready(w.event.events & 0xffff));
    auto read = f.read_generation(), write = f.write_generation();
    auto available = current;
    if (w.event.events & 0x80000000u) {
      available = current & ~w.last;
      if (read != w.read_generation) available |= current & poll_in;
      if (write != w.write_generation) available |= current & poll_out;
    }
    if (consume) {
      w.last = current;
      w.read_generation = read;
      w.write_generation = write;
    }
    return available;
  }
  short ready(short e) override {
    std::lock_guard lock(mutex);
    for (auto& [fd, w] : watches)
      if (auto f = w.file.lock(); f && !w.disabled && pending(w, *f, false)) return e & poll_in;
    return 0;
  }
};
int epoll_create_fn(int flags) {
  if (flags & ~o_cloexec) return failure(err::einval);
  auto f = std::make_shared<Epoll>();
  return result(FdTable::get().insert(f, 0, flags));
}
int epoll_legacy(int size) { return size > 0 ? epoll_create_fn(0) : failure(err::einval); }
int epoll_ctl_fn(int epfd, int op, int fd, const EpollEvent* e) {
  auto ep = std::dynamic_pointer_cast<Epoll>(FdTable::get().file(epfd));
  if (!ep) return failure(FdTable::get().file(epfd) ? err::einval : err::ebadf);
  auto f = FdTable::get().file(fd);
  if (!f) return failure(err::ebadf);
  if (f == ep) return failure(err::einval);
  if (!f->epoll_supported()) return failure(err::eperm);
  if (std::dynamic_pointer_cast<Epoll>(f))
    return failure(err::eloop);  // Nested epoll needs graph cycle tracking.
  std::lock_guard lock(ep->mutex);
  Epoll::Key key{fd, reinterpret_cast<std::uintptr_t>(f.get())};
  auto i = ep->watches.find(key);
  if (i != ep->watches.end() && i->second.file.expired()) {
    ep->watches.erase(i);
    ++ep->changes;
    i = ep->watches.end();
  }
  if (op == 2) {
    if (i == ep->watches.end()) return failure(err::enoent);
    ep->watches.erase(i);
    ++ep->changes;
    ep->readiness.notify(poll_in);
    return 0;
  }
  if (op != 1 && op != 3) return failure(err::einval);
  if (!e) return failure(err::efault);
  if (op == 1 && i != ep->watches.end()) return failure(err::eexist);
  if (op == 3 && i == ep->watches.end()) return failure(err::enoent);
  ep->watches[key] = {f, *e, 0, false, f->read_generation(), f->write_generation()};
  ++ep->changes;
  ep->readiness.notify(poll_in);
  return 0;
}
int epoll_wait_fn(int epfd, EpollEvent* out, int max, int timeout) {
  if (max <= 0 || !out) return failure(err::einval);
  auto ep = std::dynamic_pointer_cast<Epoll>(FdTable::get().file(epfd));
  if (!ep) return failure(FdTable::get().file(epfd) ? err::einval : err::ebadf);
  auto end = timeout < 0 ? INT64_MAX : monotonic_ns() + timeout * 1000000ll;
  sync_census::IoScope trace(sync_census::IoSource::epoll, (sync_census::enabled && GuestThread::current() ? GuestThread::current()->tid() : 0), end == INT64_MAX ? -1 : end);
  auto scan = [&] {
    int count = 0;
    {
      std::lock_guard lock(ep->mutex);
      if (std::erase_if(ep->watches, [](const auto& item) { return item.second.file.expired(); })) ++ep->changes;
      auto it = ep->watches.upper_bound(ep->cursor);
      for (std::size_t visited = 0; visited < ep->watches.size(); ++visited) {
        if (it == ep->watches.end()) it = ep->watches.begin();
        auto& [key, w] = *it++;
        auto f = w.file.lock();
        if (!f || w.disabled) continue;
        auto ready = Epoll::pending(w, *f, true);
        if (ready) {
          if (sync_census::enabled && !count) trace.readiness(f->readiness_time(ready));
          ep->cursor = key;
          out[count++] = {static_cast<std::uint32_t>(ready), 0, w.event.data};
          if (w.event.events & 0x40000000u) {
            w.disabled = true; ++ep->changes;
            ep->readiness.notify(poll_in);
          }
          if (count == max) break;
        }
      }
    }
    return count;
  };
  ReadinessWaitSet wait;
  std::uint64_t registered = UINT64_MAX;
  for (;;) {
    if (FdTable::get().file(epfd) != ep) return failure(err::ebadf);
    auto count = scan();
    if (count || monotonic_ns() >= end) {
      if (!count) trace.timeout();
      return count;
    }
    const auto changes = ep->changes.load();
    if (registered != changes) wait.clear();
    else wait.reset_limits();
    const auto sequence = wait.sequence();
    if (const auto status = FdTable::get().watch(epfd, poll_in, wait)) return result(status);
    registered = changes;
    count = scan();
    if (count) return count;
    if (ep->changes.load() != registered) continue;
    const auto sleep_begin = sync_census::enabled ? monotonic_ns() : 0;
    int r = wait.wait(*GuestThread::current(), sequence, end);
    if (sync_census::enabled) trace.slept(sleep_begin, monotonic_ns());
    if (r && r != -err::etimedout) return result(r);
  }
}
int epoll_pwait_fn(int fd, EpollEvent* e, int n, int timeout, const std::uint64_t* set) {
  auto& t = *GuestThread::current();
  std::uint64_t old;
  if (set) signals::set_mask(t, 2, set, &old);
  int r = epoll_wait_fn(fd, e, n, timeout);
  if (set) signals::set_mask(t, 2, &old, nullptr);
  return r;
}
// VENTURE_TRACE_NET: lookups, connects and per-call socket byte counts, for
// diagnosing guest network clients whose own logs are unavailable. Payloads
// are never logged.
const bool trace_net = environment::getenv("VENTURE_TRACE_NET") != nullptr;
std::string trace_address(const void* a, std::uint32_t n) {
  if (!a || n < 4) return "?";
  const auto* b = static_cast<const unsigned char*>(a);
  const int family = b[0] | (b[1] << 8);
  const unsigned port = (b[2] << 8) | b[3];
  char text[64] = "?";
  if (family == 2 && n >= 8) host::inet_ntop(2, b + 4, text, sizeof(text));
  else if (family == 10 && n >= 24) host::inet_ntop(10, b + 8, text, sizeof(text));
  else return "family " + std::to_string(family);
  return std::string(text) + ":" + std::to_string(port);
}
std::shared_ptr<SocketFile> socket_file(int fd) {
  return std::dynamic_pointer_cast<SocketFile>(FdTable::get().file(fd));
}
int socket_error(int fd) { return failure(FdTable::get().file(fd) ? err::enotsock : err::ebadf); }
int socket_fn(int family, int type, int protocol) {
  int error;
  auto f = host::socket(family, type & 0xf, protocol, &error);
  if (!f) return result(error);
  if (type & ~(0xf | o_cloexec | o_nonblock)) return failure(err::einval);
  f->flags = 2 | (type & o_nonblock);
  return result(FdTable::get().insert(f, 0, type & o_cloexec));
}
int bind_fn(int fd, const void* addr, std::uint32_t size) {
  auto f = socket_file(fd);
  return f ? result(f->bind(addr, size)) : socket_error(fd);
}
int listen_fn(int fd, int backlog) {
  auto f = socket_file(fd);
  return f ? result(f->listen(backlog)) : socket_error(fd);
}
int connect_fn(int fd, const void* a, std::uint32_t n) {
  auto f = socket_file(fd);
  if (!f) return socket_error(fd);
  int r = f->connect(a, n);
  if (trace_net) log::info("net-trace", "connect fd={} {} -> {}", fd, trace_address(a, n), r);
  if (r != -err::einprogress || (f->flags.load() & o_nonblock)) return result(r);
  auto end = f->operation_deadline(true);
  while (!(f->ready(poll_out) & (poll_out | poll_err | poll_hup))) {
    r = f->wait_ready(poll_out, end);
    if (r == -err::eagain) r = -err::einprogress;
    if (r) return result(r);
  }
  int error = 0;
  std::uint32_t size = 4;
  r = f->option(false, 1, 4, &error, &size);
  return result(r ? r : -error);
}
int accept4_fn(int fd, void* a, std::uint32_t* n, int flags) {
  if (flags & ~(o_cloexec | o_nonblock)) return failure(err::einval);
  auto f = socket_file(fd);
  if (!f) return socket_error(fd);
  auto end = f->operation_deadline(false);
  for (;;) {
    int r;
    auto accepted = f->accept(a, n, &r);
    if (accepted) {
      accepted->flags = 2 | (flags & o_nonblock);
      return result(FdTable::get().insert(accepted, 0, flags & o_cloexec));
    }
    if (r != -err::eagain || (f->flags.load() & o_nonblock)) return result(r);
    r = f->wait_ready(poll_in, end);
    if (r) return result(r);
  }
}
int accept_fn(int fd, void* a, std::uint32_t* n) { return accept4_fn(fd, a, n, 0); }
std::int64_t sendto_fn(int fd, const void* p, std::uint64_t n, int flags, const void* a,
                       std::uint32_t size) {
  auto f = socket_file(fd);
  if (!f) return socket_error(fd);
  auto end = f->operation_deadline(true);
  for (;;) {
    auto r = f->send(p, n, flags | 0x40, a, size);
    if (trace_net && r != -err::eagain) log::info("net-trace", "send fd={} n={} -> {}", fd, n, r);
    if (r != -err::eagain || ((f->flags.load() & o_nonblock) || (flags & 0x40))) {
      if (r == -err::epipe && !(flags & 0x4000))
        signals::send_to_thread(GuestThread::current()->tid(), {.signo = 13});
      return result(r);
    }
    int e = f->wait_ready(poll_out, end);
    if (e) return result(e);
  }
}
std::int64_t recvfrom_fn(int fd, void* p, std::uint64_t n, int flags, void* a,
                         std::uint32_t* size) {
  auto f = socket_file(fd);
  if (!f) return socket_error(fd);
  auto end = f->operation_deadline(false);
  int type = 0;
  std::uint32_t type_size = 4;
  f->option(false, 1, 3, &type, &type_size);
  bool waitall = (flags & 0x100) && type == 1 && !(flags & 2);
  std::uint64_t done = 0;
  for (;;) {
    auto r = f->recv(static_cast<unsigned char*>(p) + done, n - done, flags | 0x40, a, size);
    if (trace_net && r != -err::eagain) log::info("net-trace", "recv fd={} n={} -> {}", fd, n - done, r);
    if (r > 0) {
      done += r;
      if (!waitall || done == n || (f->flags.load() & o_nonblock) || (flags & 0x40)) return done;
      continue;
    }
    if (!r) return done;
    if (r != -err::eagain || (f->flags.load() & o_nonblock) || (flags & 0x40))
      return done ? static_cast<std::int64_t>(done) : result(r);
    int e = f->wait_ready(poll_in, end);
    if (e) return done ? static_cast<std::int64_t>(done) : result(e);
  }
}
std::int64_t send_fn(int fd, const void* p, std::uint64_t n, int flags) {
  return sendto_fn(fd, p, n, flags, nullptr, 0);
}
std::int64_t recvfrom_chk(int fd, void* p, std::uint64_t n, std::uint64_t capacity,
                        int flags, void* address, std::uint32_t* length) {
  // bionic/libc/bionic/fortify.cpp: capacity precedes flags (unlike recvfrom).
  if (n > capacity) fatal("recvfrom: prevented write past buffer");
  return recvfrom_fn(fd, p, n, flags, address, length);
}
std::int64_t recv_fn(int fd, void* p, std::uint64_t n, int flags) {
  return recvfrom_fn(fd, p, n, flags, nullptr, nullptr);
}
int shutdown_fn(int fd, int how) {
  auto f = socket_file(fd);
  if (trace_net) log::info("net-trace", "shutdown fd={} how={}", fd, how);
  return f ? result(f->shutdown(how)) : socket_error(fd);
}
int getsockopt_fn(int fd, int level, int opt, void* value, std::uint32_t* n) {
  auto f = socket_file(fd);
  if (f && level == 1 && (opt == 20 || opt == 21)) {
    if (!value || !n) return failure(err::efault);
    auto timeout = opt == 20 ? f->receive_timeout.load() : f->send_timeout.load();
    Timeval t{timeout / 1000000000, (timeout / 1000) % 1000000};
    auto count = std::min<std::uint32_t>(*n, sizeof(t));
    std::memcpy(value, &t, count);
    *n = count;
    return 0;
  }
  return f ? result(f->option(false, level, opt, value, n)) : socket_error(fd);
}
int setsockopt_fn(int fd, int level, int opt, const void* value, std::uint32_t n) {
  auto f = socket_file(fd);
  if (f && level == 1 && (opt == 20 || opt == 21)) {
    if (!value || n < 16) return failure(err::einval);
    auto t = *static_cast<const Timeval*>(value);
    if (t.sec < 0 || t.usec < 0 || t.usec >= 1000000 ||
        t.sec > (INT64_MAX - t.usec * 1000) / 1000000000)
      return failure(err::edom);
    (opt == 20 ? f->receive_timeout : f->send_timeout).store(t.sec * 1000000000 + t.usec * 1000);
    return 0;
  }
  return f ? result(f->option(true, level, opt, const_cast<void*>(value), &n)) : socket_error(fd);
}
int getsockname_fn(int fd, void* p, std::uint32_t* n) {
  auto f = socket_file(fd);
  return f ? result(f->name(false, p, n)) : socket_error(fd);
}
int getpeername_fn(int fd, void* p, std::uint32_t* n) {
  auto f = socket_file(fd);
  return f ? result(f->name(true, p, n)) : socket_error(fd);
}
// bionic netdb.h: canonname precedes addr (opposite to glibc).
struct Addrinfo {
  std::int32_t flags, family, type, protocol;
  std::uint32_t length, pad;
  GuestAddr canonical, address, next;
};
static_assert(sizeof(Addrinfo) == 48 && offsetof(Addrinfo, address) == 32);
int getaddrinfo_fn(const char* node, const char* service, const Addrinfo* hints,
                   Addrinfo** resultp) {
  if (!resultp) {
    failure(err::efault);
    return 11;
  }
  *resultp = nullptr;
  std::vector<host::Lookup> entries;
  int r = lookup_interruptible(node, service, hints ? hints->family : 0, hints ? hints->type : 0,
                               hints ? hints->protocol : 0, hints ? hints->flags : 0, entries);
  if (trace_net)
    log::info("net-trace", "getaddrinfo {} {} -> {} entries={}", node ? node : "(null)",
              service ? service : "(null)", r, entries.size());
  if (r) return r;
  Addrinfo* first = nullptr;
  Addrinfo* last = nullptr;
  for (auto& e : entries) {
    auto* a = new Addrinfo{};
    a->family = e.family;
    a->type = e.type;
    a->protocol = e.protocol;
    a->length = e.address.size;
    auto* address = new unsigned char[a->length];
    std::memcpy(address, e.address.bytes.data(), a->length);
    a->address = h2g(address);
    if (!e.canonical.empty()) {
      auto* s = new char[e.canonical.size() + 1];
      std::strcpy(s, e.canonical.c_str());
      a->canonical = h2g(s);
    }
    if (last)
      last->next = h2g(a);
    else
      first = a;
    last = a;
  }
  *resultp = first;
  return 0;
}
void freeaddrinfo_fn(Addrinfo* a) {
  while (a) {
    auto next = g2h<Addrinfo>(a->next);
    delete[] g2h<unsigned char>(a->address);
    delete[] g2h<char>(a->canonical);
    delete a;
    a = next;
  }
}
// bionic netdb.h uses positive EAI values, unlike glibc.
const char* gai_strerror_fn(int e) {
  switch (e) {
    case 0:
      return "Success";
    case 8:
      return "Name or service not known";
    case 2:
      return "Temporary failure in name resolution";
    case 4:
      return "Non-recoverable name resolution failure";
    case 5:
      return "Address family not supported";
    case 10:
      return "Socket type not supported";
    case 9:
      return "Service not supported";
    case 6:
      return "Out of memory";
    default:
      return "Address resolution error";
  }
}
int getnameinfo_fn(const void* a, std::uint32_t n, char* h, std::uint32_t hn, char* s,
                   std::uint32_t sn, int flags) {
  if (!a || n > 128) return 5;
  struct Job {
    std::atomic<bool> done{false};
    int result = 0;
    SocketAddress address;
    std::vector<char> host, service;
  };
  auto job = std::make_shared<Job>();
  job->address.size = n;
  std::memcpy(job->address.bytes.data(), a, n);
  job->host.resize(h ? hn : 0);
  job->service.resize(s ? sn : 0);
  int r = resolve_async(job, [flags](Job& j) {
    return host::reverse(j.address.bytes.data(), j.address.size,
                         j.host.empty() ? nullptr : j.host.data(), j.host.size(),
                         j.service.empty() ? nullptr : j.service.data(), j.service.size(), flags);
  });
  if (!r) {
    if (h && hn) std::memcpy(h, job->host.data(), hn);
    if (s && sn) std::memcpy(s, job->service.data(), sn);
  }
  return r;
}
int inet_pton_fn(int family, const char* s, void* out) {
  int r = host::inet_pton(family, s, out);
  return r < 0 ? failure(err::eafnosupport) : r;
}
const char* inet_ntop_fn(int family, const void* p, char* s, std::uint32_t n) {
  auto r = host::inet_ntop(family, p, s, n);
  if (!r) failure(family == 2 || family == 10 ? err::enospc : err::eafnosupport);
  return r;
}
std::uint32_t inet_addr_fn(const char* s) {
  std::uint32_t address;
  return host::inet_pton(2, s, &address) == 1 ? address : UINT32_MAX;
}
int inet_aton_fn(const char* s, std::uint32_t* out) { return host::inet_pton(2, s, out) == 1; }
const char* inet_ntoa_fn(std::uint32_t address) {
  thread_local char text[16];
  return inet_ntop_fn(2, &address, text, sizeof(text));
}
std::uint16_t swap16(std::uint16_t v) { return std::byteswap(v); }
std::uint32_t swap32(std::uint32_t v) { return std::byteswap(v); }
// socketpair(AF_UNIX, SOCK_STREAM) uses two bounded in-process pipes. Guest
// endpoint lifetimes are open-file-description lifetimes, including dup().
class LocalSocket final : public SocketFile {
 public:
  int watch(ReadinessWaitSet& wait, short events) override {
    read_.watch(wait, events & poll_in);
    write_.watch(wait, events & poll_out);
    return 0;
  }
  LocalSocket(std::shared_ptr<PipeState> in, std::shared_ptr<PipeState> out)
      : read_(std::move(in), false), write_(std::move(out), true) {
    flags = 2;
    read_.flags = o_nonblock;
    write_.flags = 1 | o_nonblock;
  }
  int bind(const void*, std::uint32_t) override { return -err::einval; }
  int connect(const void*, std::uint32_t) override { return -err::eisconn; }
  int listen(int) override { return -err::einval; }
  std::shared_ptr<SocketFile> accept(void*, std::uint32_t*, int* e) override {
    *e = -err::einval;
    return {};
  }
  std::int64_t send(const void* p, std::uint64_t n, int f, const void* to, std::uint32_t) override {
    if (to) return -err::eisconn;
    if (f & ~(0x40 | 0x4000)) return -err::enotsup;
    return write_.write_impl(p, n, false);
  }
  std::int64_t recv(void* p, std::uint64_t n, int f, void* from, std::uint32_t* size) override {
    received_flags = 0;
    if (f & ~(2 | 0x40 | 0x100 | 0x4000)) return -err::enotsup;
    if (from && size) *size = 0;
    return read_.read_impl(p, n, f & 2);
  }
  std::int64_t read(void* p, std::uint64_t n) override {
    auto end = operation_deadline(false);
    for (;;) {
      auto r = recv(p, n, 0, nullptr, nullptr);
      if (r != -err::eagain || (flags.load() & o_nonblock)) return r;
      auto e = wait_ready(poll_in, end);
      if (e) return e;
    }
  }
  std::int64_t write(const void* p, std::uint64_t n) override {
    auto end = operation_deadline(true);
    for (;;) {
      auto r = write_.write_impl(p, n, true);
      if (r != -err::eagain || (flags.load() & o_nonblock)) return r;
      auto e = wait_ready(poll_out, end);
      if (e) return e;
    }
  }
  int shutdown(int how) override {
    if (how < 0 || how > 2) return -err::einval;
    if (how != 1) read_.end();
    if (how != 0) write_.end();
    return 0;
  }
  int option(bool set, int level, int option, void* value, std::uint32_t* size) override {
    if (!set && level == 1 && (option == 3 || option == 4) && value && size && *size >= 4) {
      *static_cast<int*>(value) = option == 3 ? 1 : 0;
      *size = 4;
      return 0;
    }
    return -err::enoprotoopt;
  }
  int name(bool, void* out, std::uint32_t* size) override {
    if (!out || !size) return -err::efault;
    std::uint16_t family = 1;
    std::memcpy(out, &family, std::min<std::uint32_t>(*size, 2));
    *size = 2;
    return 0;
  }
  short ready(short e) override { return read_.ready(e & poll_in) | write_.ready(e & poll_out); }
  std::int64_t available() override { return read_.available(); }
  std::uint64_t read_generation() const override { return read_.read_generation(); }
  std::uint64_t write_generation() const override { return write_.write_generation(); }

 private:
  PipeFile read_, write_;
};
int socketpair_fn(int domain, int type, int protocol, int* out) {
  if (domain != 1) return failure(err::eafnosupport);
  if ((type & 0xf) != 1 || protocol) return failure(err::eprotonosupport);
  if (type & ~(0xf | o_cloexec | o_nonblock)) return failure(err::einval);
  if (!out) return failure(err::efault);
  auto a = std::make_shared<PipeState>(), b = std::make_shared<PipeState>();
  auto first = std::make_shared<LocalSocket>(a, b), second = std::make_shared<LocalSocket>(b, a);
  first->flags = second->flags = 2 | (type & o_nonblock);
  int x = FdTable::get().insert(first, 0, type & o_cloexec);
  if (x < 0) return result(x);
  int y = FdTable::get().insert(second, 0, type & o_cloexec);
  if (y < 0) {
    FdTable::get().close(x);
    return result(y);
  }
  out[0] = x;
  out[1] = y;
  return 0;
}
struct Message {
  GuestAddr name;
  std::uint32_t namelen, pad;
  GuestAddr iov;
  std::uint64_t iovlen;
  GuestAddr control;
  std::uint64_t controllen;
  std::int32_t flags, pad2;
};
static_assert(sizeof(Message) == 56);
std::int64_t message_io(int fd, Message* m, int flags, bool write) {
  if (!m) return failure(err::efault);
  if (m->iovlen > 1024 || (m->iovlen && !m->iov)) return failure(err::einval);
  if (write && m->controllen) return failure(err::enotsup);
  auto* iov = g2h<Iovec>(m->iov);
  std::uint64_t n = 0;
  for (std::uint64_t i = 0; i < m->iovlen; ++i) {
    if (iov[i].length > 16 * 1024 * 1024 - n) return failure(err::emsgsize);
    n += iov[i].length;
  }
  std::vector<unsigned char> bytes(n);
  std::uint64_t offset = 0;
  if (write)
    for (std::uint64_t i = 0; i < m->iovlen; ++i) {
      std::memcpy(bytes.data() + offset, g2h(iov[i].base), iov[i].length);
      offset += iov[i].length;
    }
  auto r = write ? sendto_fn(fd, bytes.data(), n, flags, g2h(m->name), m->namelen)
                 : recvfrom_fn(fd, bytes.data(), n, flags, g2h(m->name), &m->namelen);
  if (!write && r >= 0) {
    m->flags = SocketFile::received_flags;
    m->controllen = 0;
    std::uint64_t left = r;
    for (std::uint64_t i = 0; i < m->iovlen && left; ++i) {
      auto count = std::min(left, iov[i].length);
      std::memcpy(g2h(iov[i].base), bytes.data() + offset, count);
      offset += count;
      left -= count;
    }
  }
  return r;
}
std::int64_t sendmsg_fn(int fd, Message* m, int flags) { return message_io(fd, m, flags, true); }
std::int64_t recvmsg_fn(int fd, Message* m, int flags) { return message_io(fd, m, flags, false); }
struct MultiMessage {
  Message message;
  std::uint32_t length, pad;
};
int sendmmsg_fn(int fd, MultiMessage* m, unsigned count, int flags) {
  if (count > 1024) return failure(err::einval);
  for (unsigned i = 0; i < count; ++i) {
    auto r = sendmsg_fn(fd, &m[i].message, flags);
    if (r < 0) return i ? static_cast<int>(i) : -1;
    m[i].length = r;
  }
  return count;
}
int recvmmsg_fn(int fd, MultiMessage* m, unsigned count, int flags, Timespec* timeout) {
  if (count > 1024) return failure(err::einval);
  if (timeout) {
    PollFd p{fd, poll_in, 0};
    int r = ppoll_fn(&p, 1, timeout, nullptr);
    if (r <= 0) return r;
  }
  for (unsigned i = 0; i < count; ++i) {
    auto r = recvmsg_fn(fd, &m[i].message,
                        (flags & ~0x10000) | ((i && ((flags & 0x10000) || timeout)) ? 0x40 : 0));
    if (r < 0) return i ? static_cast<int>(i) : -1;
    m[i].length = r;
  }
  return count;
}
struct Hostent {
  GuestAddr name, aliases;
  std::int32_t family, length;
  GuestAddr addresses;
};
thread_local Hostent hostent;
thread_local std::string host_name;
thread_local std::vector<std::array<unsigned char, 16>> host_addresses;
thread_local std::vector<GuestAddr> host_pointers;
thread_local GuestAddr no_aliases[1]{};
thread_local int host_errno = 0;
int* h_errno_fn() { return &host_errno; }
const char* hstrerror_fn(int e) {
  switch (e) {
    case 0:
      return "Resolver success";
    case 1:
      return "Unknown host";
    case 2:
      return "Host name lookup failure";
    case 3:
      return "Unknown server error";
    case 4:
      return "No address associated with name";
    default:
      return "Resolver error";
  }
}
Hostent* gethostbyname_fn(const char* name) {
  if (!name) {
    host_errno = 1;
    return nullptr;
  }
  std::vector<host::Lookup> entries;
  if (int r = lookup_interruptible(name, nullptr, 2, 1, 0, 0, entries)) {
    host_errno = r == 2 ? 2 : 1;
    return nullptr;
  }
  host_name = name;
  host_addresses.clear();
  for (auto& e : entries) {
    std::array<unsigned char, 16> address{};
    std::memcpy(address.data(), e.address.bytes.data() + 4, 4);
    host_addresses.push_back(address);
  }
  host_pointers.clear();
  for (auto& a : host_addresses) host_pointers.push_back(h2g(a.data()));
  host_pointers.push_back(0);
  hostent = {h2g(host_name.c_str()), h2g(no_aliases), 2, 4, h2g(host_pointers.data())};
  return &hostent;
}
Hostent* gethostbyaddr_fn(const void* address, std::uint32_t length, int family) {
  if ((family != 2 && family != 10) || length != (family == 2 ? 4 : 16)) return nullptr;
  unsigned char sa[28]{};
  std::uint16_t f = family;
  std::memcpy(sa, &f, 2);
  std::memcpy(sa + (family == 2 ? 4 : 8), address, length);
  char name[1025];
  if (getnameinfo_fn(sa, family == 2 ? 16 : 28, name, sizeof(name), nullptr, 0, 4)) return nullptr;
  host_name = name;
  host_addresses.resize(1);
  std::memcpy(host_addresses[0].data(), address, length);
  host_pointers = {h2g(host_addresses[0].data()), 0};
  hostent = {h2g(host_name.c_str()), h2g(no_aliases), family, static_cast<int>(length),
             h2g(host_pointers.data())};
  host_errno = 0;
  return &hostent;
}
struct Ifaddrs {
  GuestAddr next, name;
  std::uint32_t flags, pad;
  GuestAddr address, mask, broadcast, data;
};
static_assert(sizeof(Ifaddrs) == 56);
void freeifaddrs_fn(Ifaddrs* p) {
  while (p) {
    auto* next = g2h<Ifaddrs>(p->next);
    delete[] g2h<char>(p->name);
    delete[] g2h<unsigned char>(p->address);
    delete[] g2h<unsigned char>(p->mask);
    delete[] g2h<unsigned char>(p->broadcast);
    delete p;
    p = next;
  }
}
int getifaddrs_fn(Ifaddrs** out) {
  if (!out) return failure(err::efault);
  *out = nullptr;
  std::vector<host::Interface> entries;
  int r = host::interfaces(entries);
  if (r) return result(r);
  Ifaddrs* last = nullptr;
  for (auto& e : entries) {
    auto* p = new Ifaddrs{};
    auto* name = new char[e.name.size() + 1];
    std::strcpy(name, e.name.c_str());
    p->name = h2g(name);
    p->flags = e.flags;
    auto copy = [](const SocketAddress& a) {
      auto* p = new unsigned char[a.size];
      std::memcpy(p, a.bytes.data(), a.size);
      return h2g(p);
    };
    p->address = copy(e.address);
    p->mask = copy(e.mask);
    if (e.address.bytes[0] == 2 && (e.flags & 2)) {
      auto b = e.address;
      for (int i = 4; i < 8; ++i) b.bytes[i] |= ~e.mask.bytes[i];
      p->broadcast = copy(b);
    }
    if (last)
      last->next = h2g(p);
    else
      *out = p;
    last = p;
  }
  return 0;
}
unsigned if_nametoindex_fn(const char* name) {
  if (!name) {
    failure(err::efault);
    return 0;
  }
  std::vector<host::Interface> entries;
  int r = host::interfaces(entries);
  if (r) {
    result(r);
    return 0;
  }
  for (auto& e : entries)
    if (e.name == name) return e.index;
  failure(err::enodev);
  return 0;
}
char* if_indextoname_fn(std::uint32_t index, char* name) {
  if (!name) { failure(err::efault); return nullptr; }
  std::vector<host::Interface> entries;
  int r = host::interfaces(entries);
  if (r) { result(r); return nullptr; }
  for (const auto& e : entries) {
    if (e.index != index) continue;
    // bionic net/if.h: IF_NAMESIZE is 16, including the terminator.
    if (e.name.size() >= 16) { failure(err::enospc); return nullptr; }
    // bionic/libc/bionic/net_if.cpp pads the full IF_NAMESIZE buffer.
    std::strncpy(name, e.name.c_str(), 16);
    return name;
  }
  failure(err::enodev); // bionic preserves SIOCGIFNAME's missing-interface errno.
  return nullptr;
}
GuestAddr getservbyname_fn(const char* name, const char* protocol) {
  if (!name) { failure(err::efault); return 0; }
  host::Service value;
  const int r = host::service_by_name(name, protocol, value);
  if (r <= 0) { if (r < 0) result(r); return 0; }
  // bionic/libc/include/netdb.h: char*, char**, int (network order), char*.
  struct Servent64 { GuestAddr name, aliases; std::int32_t port, padding; GuestAddr protocol; };
  struct Servent32 { std::uint32_t name, aliases; std::int32_t port; std::uint32_t protocol; };
  static_assert(sizeof(Servent64) == 32 && sizeof(Servent32) == 16);
  const bool low = process_config().isa() == GuestIsa::a32;
  const auto width = low ? 4u : 8u;
  const auto header = low ? sizeof(Servent32) : sizeof(Servent64);
  auto bytes = header + (value.aliases.size() + 1) * width + value.name.size() + value.protocol.size() + 2;
  for (const auto& alias : value.aliases) bytes += alias.size() + 1;
  thread_local GuestStorage storage;
  auto base = storage.resize(bytes);
  auto aliases = base + header;
  auto next = aliases + (value.aliases.size() + 1) * width;
  auto put_string = [&](const std::string& s) {
    auto result = next;
    std::memcpy(g2h(next), s.c_str(), s.size() + 1);
    next += s.size() + 1;
    return result;
  };
  auto service_name = put_string(value.name), service_protocol = put_string(value.protocol);
  for (std::size_t i = 0; i <= value.aliases.size(); ++i) {
    auto address = i < value.aliases.size() ? put_string(value.aliases[i]) : 0;
    if (low) g2h<std::uint32_t>(aliases)[i] = static_cast<std::uint32_t>(address);
    else g2h<GuestAddr>(aliases)[i] = address;
  }
  if (low) *g2h<Servent32>(base) = {static_cast<std::uint32_t>(service_name), static_cast<std::uint32_t>(aliases), value.port, static_cast<std::uint32_t>(service_protocol)};
  else *g2h<Servent64>(base) = {service_name, aliases, value.port, 0, service_protocol};
  return base;
}
void fd_set_fn(int fd, std::uint64_t* set, std::uint64_t size) {
  if (fd < 0 || fd >= 1024 || static_cast<unsigned>(fd) / 8 >= size) {
    failure(err::einval);
    return;
  }
  set[fd / 64] |= 1ull << (fd % 64);
}
void fd_clear_fn(int fd, std::uint64_t* set, std::uint64_t size) {
  if (fd < 0 || fd >= 1024 || static_cast<unsigned>(fd) / 8 >= size) {
    failure(err::einval);
    return;
  }
  set[fd / 64] &= ~(1ull << (fd % 64));
}
int fd_isset_fn(int fd, const std::uint64_t* set, std::uint64_t size) {
  if (fd < 0 || fd >= 1024 || static_cast<unsigned>(fd) / 8 >= size) return failure(err::einval);
  return (set[fd / 64] >> (fd % 64)) & 1;
}
GuestAddr cmsg_next_fn(const Message* m, GuestAddr current) {
  if (!m || !current || current < m->control || m->controllen > UINT64_MAX - m->control) return 0;
  auto end = m->control + m->controllen;
  if (current > end || end - current < 16) return 0;
  auto size = *g2h<std::uint64_t>(current);
  if (size < 16 || size > UINT64_MAX - 7) return 0;
  auto step = align_up(size, 8);
  if (step > end - current || end - current - step < 16) return 0;
  auto next = current + step;
  auto next_size = *g2h<std::uint64_t>(next);
  return next_size >= 16 && next_size <= end - next ? next : 0;
}
}  // namespace
int fd_poll(PollFd* f, std::uint64_t n, int timeout, sync_census::IoSource source) {
  if (n > 32768) return -err::einval;
  if (n && !f) return -err::efault;
  auto end = timeout < 0 ? INT64_MAX : monotonic_ns() + timeout * 1000000ll;
  sync_census::IoScope trace(source, (sync_census::enabled && GuestThread::current() ? GuestThread::current()->tid() : 0), end == INT64_MAX ? -1 : end);
  auto scan = [&] {
    int ready = 0;
    for (std::uint64_t i = 0; i < n; ++i) {
      f[i].revents = 0;
      if (f[i].fd < 0) continue;
      auto file = FdTable::get().file(f[i].fd);
      f[i].revents = file ? file->ready(f[i].events) : poll_nval;
      if (f[i].revents) {
        if (sync_census::enabled && !ready) trace.readiness(file ? file->readiness_time(f[i].revents) : -1);
        ++ready;
      }
    }
    return ready;
  };
  ReadinessWaitSet wait;
  for (;;) {
    auto ready = scan();
    if (ready || monotonic_ns() >= end) {
      if (!ready) trace.timeout();
      return ready;
    }
    wait.clear();
    const auto sequence = wait.sequence();
    for (std::uint64_t i = 0; i < n; ++i) if (f[i].fd >= 0) {
      const auto status = FdTable::get().watch(f[i].fd, f[i].events, wait);
      if (status && status != -err::ebadf) return status;
    }
    // Registration precedes the final readiness check. A write/close between
    // this scan and enqueue changes the futex word and cannot be lost.
    ready = scan();
    if (ready) return ready;
    const auto sleep_begin = sync_census::enabled ? monotonic_ns() : 0;
    int r = wait.wait(*GuestThread::current(), sequence, end);
    if (sync_census::enabled) trace.slept(sleep_begin, monotonic_ns());
    if (r && r != -err::etimedout) return r;
  }
}
namespace {
int getsockopt32(int fd, int level, int opt, void* value, std::uint32_t* n) {
  if (level == 1 && (opt == 20 || opt == 21)) {
    if (!value || !n) return failure(err::efault);
    Timeval wide; std::uint32_t bytes = sizeof(wide);
    int r = getsockopt_fn(fd, level, opt, &wide, &bytes); if (r) return r;
    if (wide.sec > INT32_MAX) return failure(err::eoverflow);
    abi32::Timeval t{std::int32_t(wide.sec), std::int32_t(wide.usec)};
    auto count = std::min<std::uint32_t>(*n, sizeof(t)); std::memcpy(value, &t, count); *n = count; return 0;
  }
  return getsockopt_fn(fd, level, opt, value, n);
}
int setsockopt32(int fd, int level, int opt, const void* value, std::uint32_t n) {
  if (level == 1 && (opt == 20 || opt == 21)) {
    if (!value || n < sizeof(abi32::Timeval)) return failure(err::einval);
    auto* t = static_cast<const abi32::Timeval*>(value); Timeval wide{t->sec, t->usec};
    return setsockopt_fn(fd, level, opt, &wide, sizeof(wide));
  }
  return setsockopt_fn(fd, level, opt, value, n);
}
// bionic netdb.h: 32-byte ARM addrinfo, ai_canonname before ai_addr.
struct Addrinfo32 { std::int32_t flags, family, type, protocol; std::uint32_t length, canonical, address, next; };
void freeaddrinfo32(Addrinfo32* a) { while (a) { auto* next = g2h<Addrinfo32>(a->next); deallocate(a); a = next; } }
int getaddrinfo32(const char* node, const char* service, const Addrinfo32* hints, std::uint32_t* resultp) {
  if (!resultp) { failure(err::efault); return 11; }
  *resultp = 0;
  std::vector<host::Lookup> entries;
  int r = lookup_interruptible(node, service, hints ? hints->family : 0, hints ? hints->type : 0,
    hints ? hints->protocol : 0, hints ? hints->flags : 0, entries);
  if (r) return r;
  Addrinfo32* first = nullptr; Addrinfo32* last = nullptr;
  for (const auto& e : entries) {
    auto* a = static_cast<Addrinfo32*>(allocate(sizeof(Addrinfo32) + e.address.size + e.canonical.size() + 1));
    if (!a) { freeaddrinfo32(first); return 6; } // bionic EAI_MEMORY
    *a = {}; a->family = e.family; a->type = e.type; a->protocol = e.protocol; a->length = e.address.size;
    a->address = h2g32(a + 1); std::memcpy(g2h(a->address), e.address.bytes.data(), e.address.size);
    if (!e.canonical.empty()) { a->canonical = a->address + a->length; std::memcpy(g2h(a->canonical), e.canonical.c_str(), e.canonical.size() + 1); }
    if (last) last->next = h2g32(a); else first = a; last = a;
  }
  *resultp = h2g32(first); return 0;
}
struct Hostent32 { std::uint32_t name, aliases; std::int32_t family, length; std::uint32_t addresses; };
Hostent32* copy_hostent32(Hostent* h) {
  if (!h) return nullptr;
  thread_local GuestStorage storage;
  auto n = host_addresses.size(); auto p = storage.resize(sizeof(Hostent32) + 4 + (n + 1) * 4 + n * 16 + host_name.size() + 1);
  auto* out = g2h<Hostent32>(p);
  auto aliases = std::uint32_t(p + sizeof(Hostent32)), pointers = aliases + 4;
  auto data = pointers + std::uint32_t((n + 1) * 4), name = data + std::uint32_t(n * 16);
  *out = {name, aliases, h->family, h->length, pointers}; *g2h<std::uint32_t>(aliases) = 0;
  for (std::size_t i = 0; i < n; ++i) {
    g2h<std::uint32_t>(pointers)[i] = data + std::uint32_t(i * 16);
    std::memcpy(g2h(data + i * 16), host_addresses[i].data(), 16);
  }
  g2h<std::uint32_t>(pointers)[n] = 0; std::memcpy(g2h(name), host_name.c_str(), host_name.size() + 1); return out;
}
Hostent32* gethostbyname32(const char* name) { return copy_hostent32(gethostbyname_fn(name)); }
Hostent32* gethostbyaddr32(const void* address, std::uint32_t length, int family) { return copy_hostent32(gethostbyaddr_fn(address,length,family)); }
const char* gai_strerror32(int e) { return low_string(gai_strerror_fn(e)); }
int select32(int n, std::uint32_t* rd, std::uint32_t* wr, std::uint32_t* ex, abi32::Timeval* t) {
  if (n < 0 || n > 1024) return failure(err::einval);
  // fd_set is 32 ARM longs. Stage whole bytes so the A64 service cannot
  // clear the extra four bytes when nfds ends in an odd 32-bit word.
  std::array<std::uint64_t,16> r{}, w{}, e{}; auto bytes = ((n + 31) / 32) * 4;
  if (rd) std::memcpy(r.data(),rd,bytes); if (wr) std::memcpy(w.data(),wr,bytes); if (ex) std::memcpy(e.data(),ex,bytes);
  Timeval wide{t ? t->sec : 0, t ? t->usec : 0};
  int result = select_fn(n,rd ? r.data() : nullptr,wr ? w.data() : nullptr,ex ? e.data() : nullptr,t ? &wide : nullptr);
  if (result >= 0) {
    if (rd) std::memcpy(rd,r.data(),bytes); if (wr) std::memcpy(wr,w.data(),bytes); if (ex) std::memcpy(ex,e.data(),bytes);
    if (t) *t = {std::int32_t(wide.sec),std::int32_t(wide.usec)};
  }
  return result;
}
int ppoll32(PollFd* f, std::uint32_t n, const abi32::Timespec* t, const std::uint32_t* set) {
  const Timespec wide{t ? t->sec : 0, t ? t->nsec : 0};
  const std::uint64_t mask = set ? *set : 0;
  return ppoll_fn(f, n, t ? &wide : nullptr, set ? &mask : nullptr);
}
int pselect32(int n, std::uint32_t* rd, std::uint32_t* wr, std::uint32_t* ex,
              const abi32::Timespec* t, const std::uint32_t* set) {
  if (n < 0 || n > 1024) return failure(err::einval);
  std::array<std::uint64_t, 16> r{}, w{}, e{};
  const auto bytes = ((n + 31) / 32) * 4;
  if (rd) std::memcpy(r.data(), rd, bytes);
  if (wr) std::memcpy(w.data(), wr, bytes);
  if (ex) std::memcpy(e.data(), ex, bytes);
  const Timespec wide{t ? t->sec : 0, t ? t->nsec : 0};
  const std::uint64_t mask = set ? *set : 0;
  const auto value = pselect_fn(n, rd ? r.data() : nullptr, wr ? w.data() : nullptr,
                               ex ? e.data() : nullptr, t ? &wide : nullptr, set ? &mask : nullptr);
  if (value >= 0) {
    if (rd) std::memcpy(rd, r.data(), bytes);
    if (wr) std::memcpy(wr, w.data(), bytes);
    if (ex) std::memcpy(ex, e.data(), bytes);
  }
  return value;
}
int epoll_pwait32(int fd, EpollEvent* e, int n, int timeout, const std::uint32_t* set) {
  const std::uint64_t mask = set ? *set : 0;
  return epoll_pwait_fn(fd, e, n, timeout, set ? &mask : nullptr);
}
}
void register_net(HleLibrary& lib) {
  if (process_config().isa() == GuestIsa::a32) {
    // bionic poll.h / linux/{socket,in,un}.h: pollfd is int32 + two int16;
    // sockaddr records contain fixed-width fields and socklen_t is four bytes.
    // The shared socket service already consumes Linux wire bytes, not host
    // sockaddr objects, so these records are identical for ARM32 and ARM64.
    abi32::adapt<&poll_fn>("poll");
    abi32::adapt<&ppoll32>("ppoll"); abi32::adapt<&pselect32>("pselect");
    abi32::adapt<&epoll_ctl_fn>("epoll_ctl"); abi32::adapt<&epoll_wait_fn>("epoll_wait");
    abi32::adapt<&epoll_pwait32>("epoll_pwait"); abi32::adapt<&epoll_pwait_fn>("epoll_pwait64");
    abi32::adapt<&bind_fn>("bind"); abi32::adapt<&connect_fn>("connect");
    abi32::adapt<&accept_fn>("accept"); abi32::adapt<&accept4_fn>("accept4");
    abi32::adapt<&sendto_fn>("sendto"); abi32::adapt<&recvfrom_fn>("recvfrom");
    abi32::adapt<&getsockname_fn>("getsockname"); abi32::adapt<&getpeername_fn>("getpeername");
    abi32::adapt<&getnameinfo_fn>("getnameinfo");
    abi32::adapt<&getsockopt32>("getsockopt"); abi32::adapt<&setsockopt32>("setsockopt");
    abi32::adapt<&getaddrinfo32>("getaddrinfo"); abi32::adapt<&freeaddrinfo32>("freeaddrinfo");
    abi32::adapt<&gethostbyname32>("gethostbyname"); abi32::adapt<&gethostbyaddr32>("gethostbyaddr");
    abi32::adapt<&getservbyname_fn>("getservbyname");
    abi32::adapt<&gai_strerror32>("gai_strerror"); abi32::adapt<&select32>("select");
  }
#define B(n, f) VENTURE_LIBC_BIND(lib, n, f)
  B("pipe", pipe_fn);
  B("pipe2", pipe2_fn);
  B("eventfd", eventfd_fn);
  B("eventfd_read", eventfd_read_fn);
  B("eventfd_write", eventfd_write_fn);
  B("poll", poll_fn);
  B("ppoll", ppoll_fn);
  B("__poll_chk", poll_chk);
  B("select", select_fn);
  B("pselect", pselect_fn);
  B("epoll_create1", epoll_create_fn);
  B("epoll_create", epoll_legacy);
  B("epoll_ctl", epoll_ctl_fn);
  B("epoll_wait", epoll_wait_fn);
  B("epoll_pwait", epoll_pwait_fn);
  B("epoll_pwait64", epoll_pwait_fn);
  B("socket", socket_fn);
  B("bind", bind_fn);
  B("listen", listen_fn);
  B("connect", connect_fn);
  B("accept", accept_fn);
  B("accept4", accept4_fn);
  B("send", send_fn);
  B("sendto", sendto_fn);
  B("recv", recv_fn);
  B("recvfrom", recvfrom_fn);
  B("__recvfrom_chk", recvfrom_chk);
  B("shutdown", shutdown_fn);
  B("getsockopt", getsockopt_fn);
  B("setsockopt", setsockopt_fn);
  B("getsockname", getsockname_fn);
  B("getpeername", getpeername_fn);
  B("getaddrinfo", getaddrinfo_fn);
  B("freeaddrinfo", freeaddrinfo_fn);
  B("gai_strerror", gai_strerror_fn);
  B("getnameinfo", getnameinfo_fn);
  B("inet_pton", inet_pton_fn);
  B("inet_ntop", inet_ntop_fn);
  B("inet_addr", inet_addr_fn);
  B("inet_aton", inet_aton_fn);
  B("inet_ntoa", inet_ntoa_fn);
  B("htons", swap16);
  B("ntohs", swap16);
  B("htonl", swap32);
  B("ntohl", swap32);
  B("socketpair", socketpair_fn);
  B("sendmsg", sendmsg_fn);
  B("recvmsg", recvmsg_fn);
  B("sendmmsg", sendmmsg_fn);
  B("recvmmsg", recvmmsg_fn);
  B("gethostbyname", gethostbyname_fn);
  B("gethostbyaddr", gethostbyaddr_fn);
  B("getifaddrs", getifaddrs_fn);
  B("freeifaddrs", freeifaddrs_fn);
  B("if_nametoindex", if_nametoindex_fn);
  B("if_indextoname", if_indextoname_fn);
  B("getservbyname", getservbyname_fn);
  B("__get_h_errno", h_errno_fn);
  B("hstrerror", hstrerror_fn);
  B("__FD_SET_chk", fd_set_fn);
  B("__FD_CLR_chk", fd_clear_fn);
  B("__FD_ISSET_chk", fd_isset_fn);
  B("__cmsg_nxthdr", cmsg_next_fn);
#undef B
}
}  // namespace venture::hle::libc
