#pragma once
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "fake_cpu.h"
#include "hle/ovrplatform/hle.h"

#include "../async_wait.h"

namespace venture::ovrplatform::test {
// Platform fixtures share the suite hang watchdog.
inline AppIdentity app() {
  return {"3927222113966481", "com.commutergames.vspeedwayalpha",
          "24704002429195146", 745};
}
struct Arena {
  std::atomic<int> live = 0;
  Memory memory() {
    return {[this](std::size_t size) {
              ++live;
              return h2g(::operator new(size));
            },
            [this](Handle h) {
              --live;
              ::operator delete(g2h(h));
            }};
  }
};
class FakeStore final : public SecretStore {
 public:
  Credential load() override {
    std::lock_guard lock(mutex);
    if (fail) throw std::runtime_error("fixture");
    return value;
  }
  void replace() {
    std::lock_guard lock(mutex);
    value.access_token = "fake-replaced-session";
  }
  std::mutex mutex;
  bool fail = false;
  Credential value{"fake-session-not-a-real-token", "42"};
};
class FakeProvider final : public Provider {
 public:
  Error prepare() override { return prepare_error; }
  void declare_app_id(std::string_view id) override { declared = std::string(id); }
  std::string declared;
  Result entitlement(std::stop_token stop) override {
    std::unique_lock lock(mutex);
    entered = true;
    cv.notify_all();
    cv.wait(lock, stop, [&] { return !blocked; });
    return stop.stop_requested() ? Result{failure(ErrorCode::cancelled)}
                                 : Result{};
  }
  Result logged_in_user(std::stop_token) override {
    ++user_calls;
    if (user_error) return {user_error};
    return {{}, {0xfedcba9876543210, "fixture-alias", "Fixture User"}, {}};
  }
  std::atomic<int> user_calls{0};
  Error user_error;
  Result access_token(std::stop_token) override {
    return {failure(ErrorCode::unsupported)};
  }
  void wait_entered() {
    std::unique_lock lock(mutex);
    const bool ready = cv.wait_for(lock, venture::test::hang_timeout, [&] { return entered; });
    INFO("provider entered=", entered, " blocked=", blocked);
    REQUIRE(ready);
  }
  void unblock() {
    std::lock_guard lock(mutex);
    blocked = false;
    cv.notify_all();
  }
  Error prepare_error;
  std::mutex mutex;
  std::condition_variable_any cv;
  bool entered = false, blocked = false;
};
inline const RequestSpec& spec(std::string_view name) {
  for (const auto& s : request_specs())
    if (s.name == name) return s;
  throw std::runtime_error("missing test request");
}
inline Handle wait_pop(Session& session) {
  auto deadline = std::chrono::steady_clock::now() + venture::test::hang_timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (auto message = session.pop()) return message;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const auto state = session.startup_snapshot();
  throw std::runtime_error("Platform completion timed out: lock_available=" + std::to_string(state.lock_available) +
      " state=" + std::to_string(state.state) + " pending=" + std::to_string(state.pending) +
      " queued=" + std::to_string(state.queued) + " dropped=" + std::to_string(state.dropped));
}
inline std::uint64_t invoke(Session& session, std::string_view name,
                            std::initializer_list<std::uint64_t> args = {}) {
  venture::test::FakeCpu cpu;
  std::uint64_t stack[16]{};
  cpu.use_stack(stack);
  int i = 0;
  for (auto value : args) {
    if (i < 8)
      cpu.xs[i] = value;
    else
      stack[i - 8] = value;
    ++i;
  }
  for (const auto& e : exports())
    if (name == e.name) {
      hle::FunctionInfo info{e.handler, &session, "libovrplatformloader.so",
                             e.name, 0};
      // These handlers never touch GuestThread: same test pattern as
      // hle_marshal.
      hle::CallContext ctx(*reinterpret_cast<GuestThread*>(&cpu), cpu, info);
      e.handler(ctx);
      return cpu.xs[0];
    }
  throw std::runtime_error("missing test export");
}
}  // namespace venture::ovrplatform::test
