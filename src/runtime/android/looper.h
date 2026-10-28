#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <deque>
#include <map>
#include <vector>
#include <optional>
#include "platform/profile.h"
#include "core/guest.h"

namespace venture::android {
class Looper {
 public:
  explicit Looper(int options);
  ~Looper();
  void wake();
  int add_fd(int fd, int ident, int events, GuestAddr callback, GuestAddr data);
  int remove_fd(int fd);
  int poll(int timeout_ms, int* fd, int* events, GuestAddr* data);
  void post(std::function<void()> fn, std::int64_t deadline_ns = 0);
  // In-process input sources; identity identifies a queue, readiness is checked
  // before dispatch. Source removal invalidates already collected responses.
  void add_source(GuestAddr identity, int ident, GuestAddr callback, GuestAddr data,
                  std::function<bool()> ready);
  void remove_source(GuestAddr identity);
  struct Diagnostic {
    struct Fd { int fd, ident, events; GuestAddr callback; short ready; std::int64_t unread; };
    GuestAddr sequence = 0;
    std::int32_t guest_tid = 0;
    std::uint32_t host_tid = 0;
    std::uint64_t cpu_ns = 0, calls = 0, wake_returns = 0;
    bool cpu_available = false, active = false;
    int timeout = 0, last_result = 0;
    std::vector<Fd> fds;
  };
  std::optional<Diagnostic> diagnostic();
  std::atomic<int> refs{1};

 private:
  struct Request {
    int fd, ident, events;
    GuestAddr callback, data;
    std::uint64_t generation;
    std::function<bool()> ready;
  };
  struct Response {
    GuestAddr key;
    Request request;
    int events;
  };
  std::mutex mutex_;
  int options_;
  std::map<GuestAddr, Request> requests_;
  std::deque<Response> responses_;
  std::multimap<std::int64_t, std::function<void()>> tasks_;
  // Core futex addresses must belong to AddressSpace even for a host-owned
  // looper. Use an ordinary guest futex word accessed through atomic_ref.
  GuestAddr sequence_address_ = 0;
  std::atomic_ref<std::uint32_t> sequence() const {
    return std::atomic_ref(*g2h<std::uint32_t>(sequence_address_));
  }
  bool explicit_wake_ = false;
  std::uint64_t generation_ = 0;
  int poll_impl(int timeout_ms, int* fd, int* events, GuestAddr* data);
  std::optional<platform::ProfileThread> diagnostic_owner_;
  std::int32_t diagnostic_guest_tid_ = 0;
  std::atomic<std::uint64_t> diagnostic_calls_{0}, diagnostic_wake_returns_{0};
  std::atomic<bool> diagnostic_active_{false};
  std::atomic<int> diagnostic_timeout_{0}, diagnostic_result_{0};
};
Looper* prepare_looper(int options);
Looper* thread_looper();
std::shared_ptr<Looper> pin_looper(Looper*);
void acquire_looper(Looper*);
void release_looper(Looper*);
}  // namespace venture::android
