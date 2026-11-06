#include "xr/passthrough_source.h"
#include "xr/passthrough_source_sbn1.h"
#include "common/log.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace venture::xr {
namespace sbn1 = passthrough_source_sbn1;

bool passthrough_source_answers(unsigned wait_ms) { return sbn1::answers(wait_ms); }

struct PassthroughSource::Impl {
  // Odd intent epochs request a lease; even epochs withdraw it. Epoch matching
  // prevents a late read from publishing after opt-out or a new session.
  std::atomic<std::uint64_t> intent{0}, granted_epoch{0}, lease{0}, retired_epoch{0};
  std::atomic<bool> stopping{false};
  std::mutex mutex;
  std::condition_variable changed;
  bool signalled = false;
  std::thread worker;
  std::uint64_t serial = 0;
  Impl() {
    try { worker = std::thread([this] { run(); }); } catch (...) {}
  }
  ~Impl() {
    stopping.store(true);
    wake();
    if (worker.joinable()) worker.join();
  }
  void wake() {
    { std::lock_guard lock(mutex); signalled = true; }
    changed.notify_all();
  }
  bool current(std::uint64_t epoch) const { return !stopping.load() && intent.load() == epoch; }
  void revoke() {
    const auto previous = lease.exchange(0, std::memory_order_acq_rel);
    granted_epoch.store(0);
    if (previous && (intent.load() & 1) && !stopping.load())
      log::warn("xr-passthrough", "External passthrough source stopped showing the world; ordinary VR until it resumes");
  }
  void request(bool active) {
    // Without a worker the source stays quiescent.
    if (!worker.joinable()) return;
    auto previous = intent.load();
    while (bool(previous & 1) != active) {
      if (intent.compare_exchange_weak(previous, previous + 1)) { wake(); break; }
    }
  }
  std::uint64_t permission() const {
    const auto epoch = intent.load();
    if (!(epoch & 1) || granted_epoch.load() != epoch) return 0;
    const auto value = lease.load(std::memory_order_acquire);
    return intent.load() == epoch ? value : 0;
  }
  void observe(std::uint64_t epoch) {
    const auto alive = [&] { return current(epoch); };
    std::optional<sbn1::State> previous;
    const auto publish = [&](const sbn1::State& state) {
      if (previous && state == *previous) return;
      revoke();
      if (state.shown && current(epoch)) {
        granted_epoch.store(epoch);
        lease.store(++serial, std::memory_order_release);
        log::info("xr-passthrough", "External passthrough source shows the world (generation {})", state.generation);
      }
      previous = state;
    };
    sbn1::converse(alive, publish); // the connection is closed when this returns
    revoke();
  }
  void run() noexcept {
    std::uint64_t reported_epoch = 0;
    while (!stopping.load()) {
      const auto epoch = intent.load();
      if (epoch & 1) {
        try {
          if (reported_epoch != epoch) {
            reported_epoch = epoch;
            log::info("xr-passthrough", "Asking the external passthrough source to show the world; ordinary VR until it does");
          }
          observe(epoch);
        } catch (...) {}
      }
      revoke();
      const auto settled = intent.load(std::memory_order_acquire);
      if (!(settled & 1)) retired_epoch.store(settled, std::memory_order_release);
      std::unique_lock lock(mutex);
      if (settled & 1) changed.wait_for(lock, std::chrono::seconds(1), [&] { return signalled; });
      else changed.wait(lock, [&] { return signalled; });
      signalled = false;
    }
    revoke();
  }
};
PassthroughSource::PassthroughSource() : impl_(std::make_unique<Impl>()) {}
PassthroughSource::~PassthroughSource() = default;
void PassthroughSource::request(bool active) { impl_->request(active); }
std::uint64_t PassthroughSource::permission() const { return impl_->permission(); }
bool PassthroughSource::withdrawn() const {
  const auto epoch = impl_->intent.load(std::memory_order_acquire);
  return !(epoch & 1) && impl_->retired_epoch.load(std::memory_order_acquire) == epoch &&
         impl_->intent.load(std::memory_order_acquire) == epoch;
}
} // namespace venture::xr
