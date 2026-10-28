#pragma once
#include <atomic>
#include <cstdint>

namespace venture::startup_diagnostics {
extern const bool enabled;
enum class Reason { none, progress, stall };
class Trigger {
  std::atomic<std::int64_t> armed_{0};
  std::atomic<bool> progress_{false}, fired_{false};
 public:
  void arm(std::int64_t now) { std::int64_t zero = 0; armed_.compare_exchange_strong(zero, now); }
  void progress() { progress_.store(true); }
  Reason poll(std::int64_t now, std::int64_t timeout);
};
void arm();
void progress();
Reason take_snapshot_reason();
std::int64_t timeout_seconds();
}
