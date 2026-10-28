#pragma once
#include "vfs.h"
#include <array>
#include <algorithm>
namespace venture::hle::libc {
struct SocketAddress {
  std::array<std::uint8_t, 128> bytes{};
  std::uint32_t size = 0;
};
class SocketFile : public File {
 public:
  // Per-host-thread result metadata for recvmsg; concurrent readers of the
  // same socket must not overwrite one another's message flags.
  inline static thread_local int received_flags = 0;
  bool epoll_supported() const override { return true; }
  std::atomic<std::int64_t> receive_timeout{0}, send_timeout{0};
  std::int64_t operation_deadline(bool write) const {
    auto timeout = write ? send_timeout.load() : receive_timeout.load();
    auto now = monotonic_ns();
    return !timeout || timeout > INT64_MAX - now ? INT64_MAX : now + timeout;
  }
  virtual int bind(const void*, std::uint32_t) = 0;
  virtual int connect(const void*, std::uint32_t) = 0;
  virtual int listen(int) = 0;
  virtual std::shared_ptr<SocketFile> accept(void*, std::uint32_t*, int*) = 0;
  virtual std::int64_t send(const void*, std::uint64_t, int, const void*, std::uint32_t) = 0;
  virtual std::int64_t recv(void*, std::uint64_t, int, void*, std::uint32_t*) = 0;
  virtual int shutdown(int) = 0;
  virtual int option(bool, int, int, void*, std::uint32_t*) = 0;
  virtual int name(bool, void*, std::uint32_t*) = 0;
};
namespace host {
std::shared_ptr<SocketFile> socket(int family, int type, int protocol, int* error);
struct Lookup {
  int family, type, protocol;
  SocketAddress address;
  std::string canonical;
};
int lookup(const char*, const char*, int, int, int, int, std::vector<Lookup>&);
int reverse(const void*, std::uint32_t, char*, std::uint32_t, char*, std::uint32_t, int);
int inet_pton(int, const char*, void*);
const char* inet_ntop(int, const void*, char*, std::uint32_t);
struct Interface {
  std::string name;
  std::uint32_t index, flags;
  SocketAddress address, mask;
};
int interfaces(std::vector<Interface>&);
struct Service {
  std::string name, protocol;
  std::vector<std::string> aliases;
  std::uint16_t port; // network byte order, as in bionic netdb.h's servent
};
int service_by_name(const char* name, const char* protocol, Service&); // 1 found, 0 absent, -Linux errno
}  // namespace host
}  // namespace venture::hle::libc
