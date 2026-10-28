#include "hle/libc/vfs_net.h"
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <cstdlib>
#include <map>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <algorithm>
#include <thread>
#include "libc_errno.h"
namespace venture::hle::libc::host {
int service_by_name(const char* name, const char* protocol, Service& out) {
  static std::mutex lock;
  std::lock_guard guard(lock);
  auto* value = ::getservbyname(name, protocol);
  if (!value) return 0;
  out = {value->s_name, value->s_proto, {}, static_cast<std::uint16_t>(value->s_port)};
  if (value->s_aliases) for (auto** a = value->s_aliases; *a; ++a) out.aliases.emplace_back(*a);
  return 1;
}
namespace {
// Linux x64 and Android arm64 sockaddr_in/in6 have identical layouts and
// family values. The resolver flags and EAI errors do NOT match bionic.
int gai_error(int e) {
  switch (e) {
    case 0:
      return 0;
    case EAI_AGAIN:
      return 2;
    case EAI_BADFLAGS:
      return 3;
    case EAI_FAIL:
      return 4;
    case EAI_FAMILY:
      return 5;
    case EAI_MEMORY:
      return 6;
    case EAI_NONAME:
      return 8;
    case EAI_SERVICE:
      return 9;
    case EAI_SOCKTYPE:
      return 10;
    case EAI_SYSTEM:
      return 11;
    case EAI_OVERFLOW:
      return 14;
    default:
      return 4;
  }
}
bool valid_address(const void* p, std::uint32_t n) {
  if (!p || n < 2) return false;
  auto family = static_cast<const sockaddr*>(p)->sa_family;
  return (family == AF_INET && n >= sizeof(sockaddr_in)) ||
         (family == AF_INET6 && n >= sizeof(sockaddr_in6));
}
class SocketReadiness : public std::enable_shared_from_this<SocketReadiness> {
  // One native epoll waiter serves all host sockets. Queues retain only weak
  // monitor references, so closing a socket cannot be delayed by a subscription.
  class Reactor {
    std::mutex mutex_;
    std::map<std::uint64_t, std::weak_ptr<SocketReadiness>> entries_;
    std::uint64_t next_ = 1;
    int epoll_ = -1, control_ = -1, status_ = 0;
    bool stopped_ = false;
    std::thread worker_;
    void run() noexcept {
      epoll_event events[64];
      for (;;) {
        const auto count = ::epoll_wait(epoll_, events, 64, -1);
        if (count < 0) {
          if (errno == EINTR) continue;
          std::lock_guard lock(mutex_);
          status_ = -err::eio;
          for (const auto& [id, weak] : entries_) if (auto owner = weak.lock()) {
            owner->status_ = status_;
            owner->source.notify(poll_err);
          }
          return;
        }
        for (int i = 0; i < count; ++i) {
          if (!events[i].data.u64) {
            std::uint64_t value;
            while (::read(control_, &value, sizeof(value)) < 0 && errno == EINTR) {}
            std::lock_guard lock(mutex_);
            if (stopped_) return;
            continue;
          }
          std::shared_ptr<SocketReadiness> owner;
          {
            std::lock_guard lock(mutex_);
            const auto found = entries_.find(events[i].data.u64);
            if (found != entries_.end()) owner = found->second.lock();
          }
          if (owner) owner->source.notify(events[i].events);
        }
      }
    }
   public:
    Reactor() {
      epoll_ = ::epoll_create1(EPOLL_CLOEXEC);
      if (epoll_ >= 0) control_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
      epoll_event event{}; event.events = EPOLLIN;
      if (epoll_ < 0 || control_ < 0 || ::epoll_ctl(epoll_, EPOLL_CTL_ADD, control_, &event)) {
        status_ = native_error();
      } else {
        try { worker_ = std::thread([this] { run(); }); }
        catch (...) { status_ = -err::enomem; }
      }
      if (status_) {
        if (control_ >= 0) ::close(control_);
        if (epoll_ >= 0) ::close(epoll_);
        control_ = epoll_ = -1;
      }
    }
    int update(int fd, bool enabled, std::uint32_t events, std::uint64_t& token,
               const std::weak_ptr<SocketReadiness>& owner) {
      std::lock_guard lock(mutex_);
      if (!enabled) {
        if (token) {
          if (epoll_ >= 0) ::epoll_ctl(epoll_, EPOLL_CTL_DEL, fd, nullptr);
          entries_.erase(token); token = 0;
        }
        return 0;
      }
      if (status_ || stopped_) return status_ ? status_ : -err::eio;
      epoll_event event{}; event.events = events | EPOLLET;
      const bool adding = !token;
      const auto id = adding ? next_++ : token;
      event.data.u64 = id;
      if (adding) entries_.emplace(id, owner);
      if (::epoll_ctl(epoll_, adding ? EPOLL_CTL_ADD : EPOLL_CTL_MOD, fd, &event)) {
        if (adding) entries_.erase(id);
        return native_error();
      }
      token = id;
      return 0;
    }
    void stop() noexcept {
      {
        std::lock_guard lock(mutex_);
        if (stopped_) return;
        stopped_ = true;
      }
      if (control_ >= 0) {
        const std::uint64_t one = 1;
        while (::write(control_, &one, sizeof(one)) < 0 && errno == EINTR) {}
      }
      if (worker_.joinable()) worker_.join();
      std::lock_guard lock(mutex_);
      if (control_ >= 0) ::close(control_);
      if (epoll_ >= 0) ::close(epoll_);
      control_ = epoll_ = -1;
      entries_.clear();
    }
  };
  static Reactor& reactor() {
    // Keep the registry valid for fd-table destructors after the worker stops.
    static auto* value = [] {
      auto* result = new Reactor;
      std::atexit([] { reactor().stop(); });
      return result;
    }();
    return *value;
  }
  int socket_;
  std::mutex mutex_;
  std::uint64_t token_ = 0;
  std::uint32_t events_ = 0;
  bool active_ = false, closed_ = false;
  std::atomic<int> status_{0};
  void update() noexcept {
    std::lock_guard lock(mutex_);
    if (closed_ || status_.load()) return;
    const auto wanted = source.interests();
    const auto events = wanted & ~(poll_err | poll_hup | poll_nval);
    const bool active = wanted != 0;
    if (active == active_ && events == events_) return;
    try { status_ = reactor().update(socket_, active, events, token_, weak_from_this()); }
    catch (...) { status_ = -err::enomem; }
    active_ = active; events_ = events;
    if (status_.load()) source.notify(poll_err);
  }
 public:
  ReadinessSource source;
  explicit SocketReadiness(int socket) : socket_(socket) {}
  void init() { source.set_changed([weak = weak_from_this()]() noexcept { if (auto p = weak.lock()) p->update(); }); }
  int status() const { return status_.load(); }
  void close() noexcept {
    {
      std::lock_guard lock(mutex_);
      if (closed_) return;
      closed_ = true;
      if (token_) reactor().update(socket_, false, 0, token_, {});
    }
    source.notify(UINT32_MAX);
  }
  ~SocketReadiness() { close(); }
};
class Socket final : public SocketFile {
 public:
  explicit Socket(int fd) : fd_(fd) {
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    flags = 2;
  }
  ~Socket() override { if (monitor_owner_) monitor_owner_->close(); ::close(fd_); }
  int watch(ReadinessWaitSet& wait, short events) override {
    try {
      auto* monitor = monitor_.load(std::memory_order_acquire);
      if (!monitor) {
        std::lock_guard lock(mutex);
        if (!monitor_owner_) {
          monitor_owner_ = std::make_shared<SocketReadiness>(fd_);
          monitor_owner_->init();
          monitor_.store(monitor_owner_.get(), std::memory_order_release);
        }
        monitor = monitor_owner_.get();
      }
      wait.add(monitor->source, events);
      return monitor->status();
    } catch (...) { return -err::enomem; }
  }
  int bind(const void* p, std::uint32_t n) override {
    if (!valid_address(p, n)) return -err::einval;
    return ::bind(fd_, static_cast<const sockaddr*>(p), n) ? native_error() : 0;
  }
  int connect(const void* p, std::uint32_t n) override {
    if (!valid_address(p, n)) return -err::einval;
    return ::connect(fd_, static_cast<const sockaddr*>(p), n) ? native_error() : 0;
  }
  int listen(int backlog) override { return ::listen(fd_, backlog) ? native_error() : 0; }
  std::shared_ptr<SocketFile> accept(void* p, std::uint32_t* n, int* error) override {
    auto fd = ::accept4(fd_, static_cast<sockaddr*>(p), n, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
      *error = native_error();
      return {};
    }
    ++read_epoch;
    return std::make_shared<Socket>(fd);
  }
  std::int64_t send(const void* p, std::uint64_t n, int flags, const void* a,
                    std::uint32_t len) override {
    if (a && !valid_address(a, len)) return -err::einval;
    auto r = ::sendto(fd_, p, n, flags | MSG_NOSIGNAL, static_cast<const sockaddr*>(a), len);
    if (r < 0) {
      auto e = native_error();
      if (e == -err::eagain) ++write_epoch;
      return e;
    }
    if (!(ready(poll_out) & poll_out)) ++write_epoch;
    return r;
  }
  std::int64_t recv(void* p, std::uint64_t n, int flags, void* a, std::uint32_t* len) override {
    received_flags = 0;
    iovec vector{p, n};
    msghdr message{};
    message.msg_name = a;
    message.msg_namelen = a && len ? *len : 0;
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    auto r = ::recvmsg(fd_, &message, flags);
    if (r >= 0) {
      if (a && len) *len = message.msg_namelen;
      received_flags = message.msg_flags;
    }
    if (r < 0) {
      auto e = native_error();
      if (e == -err::eagain) ++read_epoch;
      return e;
    }
    if (!(ready(poll_in) & poll_in)) ++read_epoch;
    return r;
  }
  int shutdown(int how) override {
    if (::shutdown(fd_, how)) return native_error();
    if (auto* monitor = monitor_.load(std::memory_order_acquire)) monitor->source.notify(UINT32_MAX);
    return 0;
  }
  int option(bool set, int level, int opt, void* p, std::uint32_t* n) override {
    if (!p || !n) return -err::efault;
    int r = set ? ::setsockopt(fd_, level, opt, p, *n) : ::getsockopt(fd_, level, opt, p, n);
    return r ? native_error() : 0;
  }
  int name(bool peer, void* p, std::uint32_t* n) override {
    return (peer ? ::getpeername(fd_, static_cast<sockaddr*>(p), n)
                 : ::getsockname(fd_, static_cast<sockaddr*>(p), n))
               ? native_error()
               : 0;
  }
  short ready(short e) override {
    pollfd p{fd_, e, 0};
    int r = ::poll(&p, 1, 0);
    return r < 0 ? poll_err : p.revents;
  }
  std::int64_t available() override {
    int n = 0;
    return ::ioctl(fd_, FIONREAD, &n) ? native_error() : n;
  }
  std::int64_t read(void* p, std::uint64_t n) override {
    auto end = operation_deadline(false);
    for (;;) {
      auto r = recv(p, n, 0, nullptr, nullptr);
      if (r != -err::eagain || (flags.load() & o_nonblock)) return r;
      int e = wait_ready(poll_in, end);
      if (e) return e;
    }
  }
  std::int64_t write(const void* p, std::uint64_t n) override {
    auto end = operation_deadline(true);
    for (;;) {
      auto r = send(p, n, 0, nullptr, 0);
      if (r != -err::eagain || (flags.load() & o_nonblock)) return r;
      int e = wait_ready(poll_out, end);
      if (e) return e;
    }
  }

 private:
  int fd_;
  std::shared_ptr<SocketReadiness> monitor_owner_;
  std::atomic<SocketReadiness*> monitor_{nullptr};
};
}  // namespace
std::shared_ptr<SocketFile> socket(int family, int type, int protocol, int* error) {
  if (family != 2 && family != 10) {
    *error = -err::eafnosupport;
    return {};
  }
  auto fd = ::socket(family, type | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
  if (fd < 0) {
    *error = native_error();
    return {};
  }
  return std::make_shared<Socket>(fd);
}
int lookup(const char* node, const char* service, int family, int type, int protocol, int flags,
           std::vector<Lookup>& out) {
  addrinfo hints{};
  hints.ai_family = family;
  hints.ai_socktype = type;
  hints.ai_protocol = protocol;
  if (flags & ~(1 | 2 | 4 | 8 | 0x100 | 0x200 | 0x400 | 0x800)) return 3;
  hints.ai_flags = ((flags & 1) ? AI_PASSIVE : 0) | ((flags & 2) ? AI_CANONNAME : 0) |
                   ((flags & 4) ? AI_NUMERICHOST : 0) | ((flags & 8) ? AI_NUMERICSERV : 0) |
                   ((flags & 0x100) ? AI_ALL : 0) | ((flags & (0x200 | 0x800)) ? AI_V4MAPPED : 0) |
                   ((flags & 0x400) ? AI_ADDRCONFIG : 0);
  addrinfo* result;
  int r = ::getaddrinfo(node, service, &hints, &result);
  if (r) return gai_error(r);
  for (auto* a = result; a; a = a->ai_next) {
    if (a->ai_addrlen > 128) continue;
    Lookup e{};
    e.family = a->ai_family;
    e.type = a->ai_socktype;
    e.protocol = a->ai_protocol;
    e.address.size = a->ai_addrlen;
    std::memcpy(e.address.bytes.data(), a->ai_addr, a->ai_addrlen);
    if (a->ai_canonname) e.canonical = a->ai_canonname;
    out.push_back(std::move(e));
  }
  ::freeaddrinfo(result);
  return 0;
}
int reverse(const void* p, std::uint32_t n, char* h, std::uint32_t hn, char* s, std::uint32_t sn,
            int flags) {
  int f = ((flags & 1) ? NI_NOFQDN : 0) | ((flags & 2) ? NI_NUMERICHOST : 0) |
          ((flags & 4) ? NI_NAMEREQD : 0) | ((flags & 8) ? NI_NUMERICSERV : 0) |
          ((flags & 16) ? NI_DGRAM : 0);
  return gai_error(::getnameinfo(static_cast<const sockaddr*>(p), n, h, hn, s, sn, f));
}
int inet_pton(int family, const char* s, void* p) { return ::inet_pton(family, s, p); }
const char* inet_ntop(int family, const void* p, char* s, std::uint32_t n) {
  return ::inet_ntop(family, p, s, n);
}
int interfaces(std::vector<Interface>& out) {
  ifaddrs* list;
  if (::getifaddrs(&list)) return native_error();
  for (auto* a = list; a; a = a->ifa_next) {
    if (!a->ifa_addr || (a->ifa_addr->sa_family != AF_INET && a->ifa_addr->sa_family != AF_INET6))
      continue;
    Interface e;
    e.name = a->ifa_name;
    e.index = ::if_nametoindex(a->ifa_name);
    e.flags = a->ifa_flags;
    e.address.size = e.mask.size = a->ifa_addr->sa_family == AF_INET ? 16 : 28;
    std::memcpy(e.address.bytes.data(), a->ifa_addr, e.address.size);
    if (a->ifa_netmask) std::memcpy(e.mask.bytes.data(), a->ifa_netmask, e.mask.size);
    out.push_back(std::move(e));
  }
  ::freeifaddrs(list);
  return 0;
}
}  // namespace venture::hle::libc::host
