#pragma once
#include <cstdint>
#include <memory>
namespace venture::platform {
// Request normal teardown of an owned child; returns false if it has no handler.
bool request_runtime_stop(std::uint32_t pid);
// Process-local console/signal handling and launcher lifetime monitoring.
// Does not install a machine-wide hook or terminate another process.
class RuntimeStop {
 public:
  explicit RuntimeStop(std::uint32_t parent_pid);
  ~RuntimeStop();
  bool requested() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
