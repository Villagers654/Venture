#pragma once
#include "hle/ovrplatform/session.h"
#include "common/json.h"
#include <deque>
#include <mutex>

namespace venture::ovrplatform {
struct HorizonMessage {
  Request request_id;
  std::int32_t status_code, cookie;
  std::string status_message, data;
};
// JSON/JNI facade over the same Session handlers used by the native SDK.
// Session must outlive this object; all networking remains in host services.
class Horizon {
 public:
  explicit Horizon(Session& session) : session_(session) {}
  InitResult setup(std::string_view app_id);
  std::uint64_t request(std::string_view module, std::string_view method,
                        std::int32_t version, std::string_view data, std::int32_t cookie,
                        bool subscription = false);
  std::optional<HorizonMessage> pop(std::uint64_t session);
  std::size_t count(std::uint64_t session);
  void stop(std::uint64_t session);
  static constexpr std::uint64_t global_session = 1;
 private:
  struct Pending { Request request; std::uint64_t session; std::int32_t cookie; const char* operation; };
  struct Ready { std::uint64_t session; HorizonMessage message; };
  void collect();
  Session& session_;
  std::mutex mutex_;
  std::uint64_t next_session_ = 2;
  std::vector<Pending> pending_;
  std::deque<Ready> ready_;
};
}  // namespace venture::ovrplatform
