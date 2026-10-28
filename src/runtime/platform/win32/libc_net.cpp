#include "hle/libc/vfs_net.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <algorithm>
#include <cstring>

namespace venture::hle::libc::host {
namespace {
int error(int e = WSAGetLastError()) {
  switch (e) {
    case 0:
      return 0;
    case WSAEINTR:
      return -err::eintr;
    case WSAEACCES:
      return -err::eacces;
    case WSAEFAULT:
      return -err::efault;
    case WSAEINVAL:
      return -err::einval;
    case WSAEMFILE:
      return -err::emfile;
    case WSAEWOULDBLOCK:
      return -err::eagain;
    case WSAEINPROGRESS:
      return -err::einprogress;
    case WSAEALREADY:
      return -err::ealready;
    case WSAENOTSOCK:
      return -err::enotsock;
    case WSAEDESTADDRREQ:
      return -err::edestaddrreq;
    case WSAEMSGSIZE:
      return -err::emsgsize;
    case WSAEPROTOTYPE:
      return -err::eprototype;
    case WSAENOPROTOOPT:
      return -err::enoprotoopt;
    case WSAEPROTONOSUPPORT:
      return -err::eprotonosupport;
    case WSAEOPNOTSUPP:
      return -err::eopnotsupp;
    case WSAEAFNOSUPPORT:
      return -err::eafnosupport;
    case WSAEADDRINUSE:
      return -err::eaddrinuse;
    case WSAEADDRNOTAVAIL:
      return -err::eaddrnotavail;
    case WSAENETDOWN:
      return -err::enetdown;
    case WSAENETUNREACH:
      return -err::enetunreach;
    case WSAENETRESET:
      return -err::enetreset;
    case WSAECONNABORTED:
      return -err::econnaborted;
    case WSAECONNRESET:
      return -err::econnreset;
    case WSAENOBUFS:
      return -err::enobufs;
    case WSAEISCONN:
      return -err::eisconn;
    case WSAENOTCONN:
      return -err::enotconn;
    case WSAESHUTDOWN:
      return -err::epipe;
    case WSAETIMEDOUT:
      return -err::etimedout;
    case WSAECONNREFUSED:
      return -err::econnrefused;
    case WSAEHOSTUNREACH:
      return -err::ehostunreach;
    default:
      return -err::eio;
  }
}
int startup() {
  static int status = [] {
    WSADATA w{};
    return WSAStartup(MAKEWORD(2, 2), &w);
  }();
  return status ? -err::enetdown : 0;
}
int address_in(const void* in, std::uint32_t n, sockaddr_storage& out, int& length) {
  if (!in || n < 2) return -err::einval;
  std::uint16_t family;
  std::memcpy(&family, in, 2);
  if (family != 2 && family != 10) return -err::eafnosupport;
  length = family == 2 ? 16 : 28;
  if (n < static_cast<unsigned>(length)) return -err::einval;
  std::memset(&out, 0, sizeof(out));
  std::memcpy(&out, in, length);
  out.ss_family = family == 2 ? AF_INET : AF_INET6;
  return 0;
}
int address_out(const sockaddr* in, int length, void* out, std::uint32_t* size) {
  if (!out) return 0;
  if (!size) return -err::efault;
  sockaddr_storage copy{};
  if (length < 0 || length > sizeof(copy)) return -err::einval;
  std::memcpy(&copy, in, length);
  copy.ss_family = in->sa_family == AF_INET6 ? 10 : in->sa_family;
  std::memcpy(out, &copy, std::min<std::uint32_t>(*size, length));
  *size = length;
  return 0;
}
int message_flags(int flags) {
  return ((flags & 1) ? MSG_OOB : 0) | ((flags & 2) ? MSG_PEEK : 0) |
         ((flags & 4) ? MSG_DONTROUTE : 0);
}
class SocketReadiness : public std::enable_shared_from_this<SocketReadiness> {
  SOCKET socket_;
  std::mutex mutex_;
  WSAEVENT event_ = WSA_INVALID_EVENT;
  PTP_WAIT wait_ = nullptr;
  long mask_ = 0;
  bool closed_ = false;
  std::atomic<int> status_{0};
  static void CALLBACK callback(PTP_CALLBACK_INSTANCE, void* context, PTP_WAIT, TP_WAIT_RESULT) {
    static_cast<SocketReadiness*>(context)->ready();
  }
  void ready() noexcept {
    std::uint32_t events = 0;
    {
      std::lock_guard lock(mutex_);
      if (closed_ || !mask_) return;
      WSANETWORKEVENTS native{};
      if (WSAEnumNetworkEvents(socket_, event_, &native)) {
        status_ = -err::eio;
        events = poll_err;
      } else {
        if (native.lNetworkEvents & (FD_READ | FD_ACCEPT)) events |= poll_in;
        if (native.lNetworkEvents & (FD_WRITE | FD_CONNECT)) events |= poll_out;
        if (native.lNetworkEvents & FD_OOB) events |= 2;
        if (native.lNetworkEvents & FD_CLOSE) events |= poll_in | poll_out | poll_hup;
        for (int code : native.iErrorCode) if (code) events |= poll_err;
      }
      if (!status_.load()) SetThreadpoolWait(wait_, event_, nullptr);
    }
    source.notify(events);
  }
  void update() noexcept {
    std::lock_guard lock(mutex_);
    if (closed_ || status_.load()) return;
    const auto wanted = source.interests();
    const long mask = !wanted ? 0 : FD_CLOSE | (wanted & poll_in ? FD_READ | FD_ACCEPT : 0) |
                                     (wanted & poll_out ? FD_WRITE | FD_CONNECT : 0) | (wanted & 2 ? FD_OOB : 0);
    if (mask == mask_) return;
    if (mask && !wait_) {
      event_ = WSACreateEvent();
      if (event_ != WSA_INVALID_EVENT) wait_ = CreateThreadpoolWait(callback, this, nullptr);
      if (!wait_) { status_ = -err::enomem; source.notify(poll_err); return; }
    }
    if (wait_) SetThreadpoolWait(wait_, nullptr, nullptr);
    if (event_ != WSA_INVALID_EVENT) WSAResetEvent(event_);
    if (WSAEventSelect(socket_, mask ? event_ : nullptr, mask)) { status_ = error(); source.notify(poll_err); return; }
    mask_ = mask;
    if (mask) SetThreadpoolWait(wait_, event_, nullptr);
  }
 public:
  ReadinessSource source;
  explicit SocketReadiness(SOCKET socket) : socket_(socket) {}
  void init() {
    source.set_changed([weak = weak_from_this()]() noexcept { if (auto owner = weak.lock()) owner->update(); });
  }
  int status() const { return status_.load(); }
  void close() noexcept {
    {
      std::lock_guard lock(mutex_);
      if (closed_) return;
      closed_ = true;
      if (wait_) SetThreadpoolWait(wait_, nullptr, nullptr);
      if (mask_) WSAEventSelect(socket_, nullptr, 0);
    }
    // No queue or socket mutex is held while draining callbacks.
    if (wait_) { WaitForThreadpoolWaitCallbacks(wait_, TRUE); CloseThreadpoolWait(wait_); }
    if (event_ != WSA_INVALID_EVENT) WSACloseEvent(event_);
    source.notify(UINT32_MAX);
  }
  ~SocketReadiness() { close(); }
};
class Socket final : public SocketFile {
 public:
  explicit Socket(SOCKET s) : s_(s) {
    u_long nonblock = 1;
    ioctlsocket(s_, FIONBIO, &nonblock);
    flags = 2;
  }
  ~Socket() override { if (monitor_owner_) monitor_owner_->close(); closesocket(s_); }
  int watch(ReadinessWaitSet& wait, short events) override {
    try {
      auto* monitor = monitor_.load(std::memory_order_acquire);
      if (!monitor) {
        std::lock_guard lock(mutex);
        if (!monitor_owner_) {
          monitor_owner_ = std::make_shared<SocketReadiness>(s_);
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
    sockaddr_storage a{};
    int len;
    int r = address_in(p, n, a, len);
    return r ? r : (::bind(s_, reinterpret_cast<sockaddr*>(&a), len) ? error() : 0);
  }
  int connect(const void* p, std::uint32_t n) override {
    sockaddr_storage a{};
    int len;
    int r = address_in(p, n, a, len);
    if (r) return r;
    if (!::connect(s_, reinterpret_cast<sockaddr*>(&a), len)) return 0;
    r = error();
    return r == -err::eagain ? -err::einprogress : r;
  }
  int listen(int backlog) override { return ::listen(s_, backlog) ? error() : 0; }
  std::shared_ptr<SocketFile> accept(void* p, std::uint32_t* size, int* e) override {
    sockaddr_storage a{};
    int n = sizeof(a);
    auto s = ::accept(s_, reinterpret_cast<sockaddr*>(&a), &n);
    if (s == INVALID_SOCKET) {
      *e = error();
      return {};
    }
    int r = address_out(reinterpret_cast<sockaddr*>(&a), n, p, size);
    if (r) {
      closesocket(s);
      *e = r;
      return {};
    }
    ++read_epoch;
    return std::make_shared<Socket>(s);
  }
  std::int64_t send(const void* p, std::uint64_t n, int f, const void* to,
                    std::uint32_t size) override {
    if (f & ~(1 | 2 | 4 | 0x40 | 0x100 | 0x4000)) return -err::eopnotsupp;
    if (n > INT_MAX) return -err::emsgsize;
    sockaddr_storage a{};
    int len = 0;
    if (to) {
      int r = address_in(to, size, a, len);
      if (r) return r;
    }
    int r = ::sendto(s_, static_cast<const char*>(p), static_cast<int>(n), message_flags(f),
                     to ? reinterpret_cast<sockaddr*>(&a) : nullptr, len);
    if (r == SOCKET_ERROR) {
      auto e = error();
      if (e == -err::eagain) ++write_epoch;
      return e;
    }
    if (!(ready(poll_out) & poll_out)) ++write_epoch;
    return r;
  }
  std::int64_t recv(void* p, std::uint64_t n, int f, void* from, std::uint32_t* size) override {
    if (f & ~(1 | 2 | 0x40 | 0x100 | 0x4000)) return -err::eopnotsupp;
    received_flags = 0;
    sockaddr_storage a{};
    int len = sizeof(a);
    WSABUF buffer{static_cast<ULONG>(std::min<std::uint64_t>(n, INT_MAX)), static_cast<char*>(p)};
    DWORD received = 0, native_flags = message_flags(f);
    int status = WSARecvFrom(s_, &buffer, 1, &received, &native_flags,
                             from ? reinterpret_cast<sockaddr*>(&a) : nullptr,
                             from ? &len : nullptr, nullptr, nullptr);
    if (status == SOCKET_ERROR) {
      auto e = error();
      if (e == -err::emsgsize)
        received_flags = 0x20;
      else {
        if (e == -err::eagain) ++read_epoch;
        return e;
      }
    }
    auto r = static_cast<std::int64_t>(received);
    if (!(ready(poll_in) & poll_in)) ++read_epoch;
    int e = address_out(reinterpret_cast<sockaddr*>(&a), len, from, size);
    return e ? e : r;
  }
  int shutdown(int how) override {
    if (how < 0 || how > 2) return -err::einval;
    if (::shutdown(s_, how)) return error();
    if (auto* monitor = monitor_.load(std::memory_order_acquire)) monitor->source.notify(UINT32_MAX);
    return 0;
  }
  int option(bool set, int level, int opt, void* value, std::uint32_t* size) override {
    if (!size || !value) return -err::efault;
    int native_level = level, native_option = opt;
    if (level == 1) {
      native_level = SOL_SOCKET;
      switch (opt) {
        case 2:
          native_option = SO_REUSEADDR;
          break;
        case 3:
          native_option = SO_TYPE;
          break;
        case 4:
          native_option = SO_ERROR;
          break;
        case 5:
          native_option = SO_DONTROUTE;
          break;
        case 6:
          native_option = SO_BROADCAST;
          break;
        case 7:
          native_option = SO_SNDBUF;
          break;
        case 8:
          native_option = SO_RCVBUF;
          break;
        case 9:
          native_option = SO_KEEPALIVE;
          break;
        case 10:
          native_option = SO_OOBINLINE;
          break;
        case 13: {
          struct GuestLinger {
            int on, seconds;
          };
          if (*size < 8) return -err::einval;
          auto* g = static_cast<GuestLinger*>(value);
          linger l{};
          int n = sizeof(l);
          if (set) {
            l.l_onoff = g->on;
            l.l_linger = g->seconds;
            return ::setsockopt(s_, SOL_SOCKET, SO_LINGER, reinterpret_cast<char*>(&l), n) ? error()
                                                                                           : 0;
          }
          int r = ::getsockopt(s_, SOL_SOCKET, SO_LINGER, reinterpret_cast<char*>(&l), &n);
          if (r) return error();
          *g = {l.l_onoff, l.l_linger};
          *size = 8;
          return 0;
        }
        default:
          return -err::enoprotoopt;
      }
    } else if (level == 6) {
      if (opt != 1) return -err::enoprotoopt;
      native_option = TCP_NODELAY;
    } else if (level == 0) {
      switch (opt) {
        case 1:
          native_option = IP_TOS;
          break;
        case 2:
          native_option = IP_TTL;
          break;
        case 32:
          native_option = IP_MULTICAST_IF;
          break;
        case 33:
          native_option = IP_MULTICAST_TTL;
          break;
        case 34:
          native_option = IP_MULTICAST_LOOP;
          break;
        case 35:
          native_option = IP_ADD_MEMBERSHIP;
          break;
        case 36:
          native_option = IP_DROP_MEMBERSHIP;
          break;
        default:
          return -err::enoprotoopt;
      }
    } else if (level == 41) {
      if (opt == 26)
        native_option = IPV6_V6ONLY;
      else
        return -err::enoprotoopt;
    } else
      return -err::enoprotoopt;
    int n = static_cast<int>(*size);
    if (set)
      return ::setsockopt(s_, native_level, native_option, static_cast<const char*>(value), n)
                 ? error()
                 : 0;
    int r = ::getsockopt(s_, native_level, native_option, static_cast<char*>(value), &n);
    if (r) return error();
    *size = n;
    if (level == 1 && opt == 4 && n >= 4)
      *static_cast<int*>(value) = -error(*static_cast<int*>(value));
    return 0;
  }
  int name(bool peer, void* p, std::uint32_t* size) override {
    sockaddr_storage a{};
    int n = sizeof(a);
    int r = peer ? ::getpeername(s_, reinterpret_cast<sockaddr*>(&a), &n)
                 : getsockname(s_, reinterpret_cast<sockaddr*>(&a), &n);
    return r ? error() : address_out(reinterpret_cast<sockaddr*>(&a), n, p, size);
  }
  short ready(short e) override {
    WSAPOLLFD p{s_, 0, 0};
    if (e & poll_in) p.events |= POLLRDNORM;
    if (e & poll_out) p.events |= POLLWRNORM;
    // WSAPoll rejects POLLPRI with WSAEINVAL. Winsock uses POLLRDBAND for
    // urgent TCP data; map Linux POLLPRI to it (MSDN WSAPoll documentation).
    if (e & 2) p.events |= POLLRDBAND;
    int r = WSAPoll(&p, 1, 0);
    if (r < 0) return poll_err;
    return ((p.revents & POLLRDNORM) ? e & poll_in : 0) |
           ((p.revents & POLLWRNORM) ? e & poll_out : 0) | ((p.revents & POLLRDBAND) ? e & 2 : 0) |
           ((p.revents & POLLERR) ? poll_err : 0) | ((p.revents & POLLHUP) ? poll_hup : 0) |
           ((p.revents & POLLNVAL) ? poll_nval : 0);
  }
  std::int64_t available() override {
    u_long n;
    return ioctlsocket(s_, FIONREAD, &n) ? error() : static_cast<std::int64_t>(n);
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
  SOCKET s_;
  std::shared_ptr<SocketReadiness> monitor_owner_;
  std::atomic<SocketReadiness*> monitor_{nullptr};
};
int gai_error(int e) {
  switch (e) {
    case 0:
      return 0;
    case WSAHOST_NOT_FOUND:
      return 8;
    case WSATRY_AGAIN:
      return 2;
    case WSANO_RECOVERY:
      return 4;
    case WSAEAFNOSUPPORT:
      return 5;
    case WSAESOCKTNOSUPPORT:
      return 10;
    case WSATYPE_NOT_FOUND:
      return 9;
    case WSA_NOT_ENOUGH_MEMORY:
      return 6;
    default:
      return 11;
  }
}
}  // namespace
std::shared_ptr<SocketFile> socket(int family, int type, int protocol, int* e) {
  *e = startup();
  if (*e) return {};
  if (family != 2 && family != 10) {
    *e = -err::eafnosupport;
    return {};
  }
  auto s = ::socket(family == 2 ? AF_INET : AF_INET6, type, protocol);
  if (s == INVALID_SOCKET) {
    *e = error();
    return {};
  }
  return std::make_shared<Socket>(s);
}
int lookup(const char* node, const char* service, int family, int type, int protocol, int flags,
           std::vector<Lookup>& out) {
  if (startup()) return 4;
  if (family != 0 && family != 2 && family != 10) return 5;
  addrinfo hint{};
  hint.ai_family = family == 10 ? AF_INET6 : family;
  hint.ai_socktype = type;
  hint.ai_protocol = protocol;
  if (flags & ~(1 | 2 | 4 | 8 | 0x100 | 0x200 | 0x400 | 0x800)) return 3;
  hint.ai_flags = ((flags & 1) ? AI_PASSIVE : 0) | ((flags & 2) ? AI_CANONNAME : 0) |
                  ((flags & 4) ? AI_NUMERICHOST : 0) |
                  ((flags & (0x800 | 0x200)) ? AI_V4MAPPED : 0) | ((flags & 0x100) ? AI_ALL : 0) |
                  ((flags & 0x400) ? AI_ADDRCONFIG : 0) | ((flags & 8) ? AI_NUMERICSERV : 0);
  addrinfo* list = nullptr;
  int r = ::getaddrinfo(node, service, &hint, &list);
  if (r) return gai_error(r);
  for (auto* a = list; a; a = a->ai_next) {
    Lookup item{};
    item.family = a->ai_family == AF_INET6 ? 10 : a->ai_family;
    item.type = a->ai_socktype;
    item.protocol = a->ai_protocol;
    item.address.size = 128;
    address_out(a->ai_addr, a->ai_addrlen, item.address.bytes.data(), &item.address.size);
    if (a->ai_canonname) item.canonical = a->ai_canonname;
    out.push_back(std::move(item));
  }
  ::freeaddrinfo(list);
  return 0;
}
int reverse(const void* p, std::uint32_t size, char* name, std::uint32_t ns, char* service,
            std::uint32_t ss, int flags) {
  if (startup()) return 4;
  sockaddr_storage a{};
  int n;
  if (address_in(p, size, a, n)) return 5;
  int f = ((flags & 2) ? NI_NUMERICHOST : 0) | ((flags & 8) ? NI_NUMERICSERV : 0) |
          ((flags & 1) ? NI_NOFQDN : 0) | ((flags & 4) ? NI_NAMEREQD : 0) |
          ((flags & 16) ? NI_DGRAM : 0);
  return gai_error(::getnameinfo(reinterpret_cast<sockaddr*>(&a), n, name, ns, service, ss, f));
}
int inet_pton(int family, const char* s, void* p) {
  if (family != 2 && family != 10) return -1;
  return ::InetPtonA(family == 10 ? AF_INET6 : AF_INET, s, p);
}
const char* inet_ntop(int family, const void* p, char* s, std::uint32_t n) {
  if (family != 2 && family != 10) return nullptr;
  return ::InetNtopA(family == 10 ? AF_INET6 : AF_INET, const_cast<void*>(p), s, n);
}
int interfaces(std::vector<Interface>& out) {
  ULONG size = 16384;
  std::vector<unsigned char> buffer(size);
  ULONG status;
  for (int tries = 0; tries < 3; ++tries) {
    status = GetAdaptersAddresses(
        AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    if (status != ERROR_BUFFER_OVERFLOW) break;
    buffer.resize(size);
  }
  if (status != NO_ERROR) return -err::eio;
  for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); a; a = a->Next)
    for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
      auto family = u->Address.lpSockaddr->sa_family;
      if (family != AF_INET && family != AF_INET6) continue;
      Interface e;
      e.index = family == AF_INET ? a->IfIndex : a->Ipv6IfIndex;
      bool loop = a->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
      e.name = loop ? "lo" : "eth" + std::to_string(a->IfIndex);
      e.flags = (loop ? 8 : 2) | (a->OperStatus == IfOperStatusUp ? 65 : 0) |
                ((a->Flags & IP_ADAPTER_NO_MULTICAST) ? 0 : 4096);
      e.address.size = 128;
      address_out(u->Address.lpSockaddr, u->Address.iSockaddrLength, e.address.bytes.data(),
                  &e.address.size);
      e.mask.size = e.address.size;
      std::uint16_t f = family == AF_INET ? 2 : 10;
      std::memcpy(e.mask.bytes.data(), &f, 2);
      auto off = family == AF_INET ? 4 : 8;
      auto bits = std::min<unsigned>(u->OnLinkPrefixLength, family == AF_INET ? 32 : 128);
      for (unsigned i = 0; i < bits; ++i) e.mask.bytes[off + i / 8] |= 1u << (7 - i % 8);
      out.push_back(std::move(e));
    }
  return 0;
}
int service_by_name(const char* name, const char* protocol, Service& out) {
  if (auto r = startup()) return r;
  // Winsock's returned servent is borrowed. Snapshot it before any other
  // service lookup; guest code only sees copies with the bionic layout.
  static std::mutex lock;
  std::lock_guard guard(lock);
  auto* value = ::getservbyname(name, protocol);
  if (!value) return 0;
  out = {value->s_name, value->s_proto, {}, static_cast<std::uint16_t>(value->s_port)};
  if (value->s_aliases) for (auto** a = value->s_aliases; *a; ++a) out.aliases.emplace_back(*a);
  return 1;
}
}  // namespace venture::hle::libc::host
