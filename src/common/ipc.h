#pragma once
#include "common/json.h"
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>

namespace venture {
// Bounded length-prefixed JSON on inherited anonymous pipes. No environment or
// command-line payloads. Each endpoint owns its two handles/descriptors.
class PipeChannel {
public:
  using Handler = std::function<Json(const Json &)>;
  static std::array<std::shared_ptr<PipeChannel>, 2> pair();
  static std::shared_ptr<PipeChannel> inherited(std::uintptr_t read,
                                                std::uintptr_t write);
  ~PipeChannel();
  Json call(const Json &request); // one outstanding RPC; thread-safe
  // The receiving side of one call(): reads a single request, hands it to
  // `handler`, writes back its result (or a generic error) and returns it.
  // For a short-lived host process that answers exactly one request, e.g.
  // the Meta session native messaging host.
  Json accept(const Handler &handler);
  void serve(const Handler &, std::stop_token);
  void close();
  std::array<std::uintptr_t, 2> handles() const {
    return {read_.load(), write_.load()};
  }

private:
  PipeChannel(std::uintptr_t read, std::uintptr_t write)
      : read_(read), write_(write) {}
  std::atomic<std::uintptr_t> read_, write_;
  std::mutex mutex_;
  void send(const Json &);
  Json receive();
};
} // namespace venture
