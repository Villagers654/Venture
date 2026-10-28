#pragma once
#include <array>
#include <atomic>
#include <cstring>
#include <string_view>

namespace venture {
// One guest/host producer and the diagnostic writer. Full means drop, never wait.
struct DiagnosticRecord { std::array<char, 1024> bytes; std::size_t size = 0; };
template<std::size_t Capacity> class SizedDiagnosticBuffer {
 public:
  using Record = DiagnosticRecord;
  bool push(std::string_view tag, std::string_view message) noexcept {
    const auto next = written_.load(std::memory_order_relaxed);
    if (next - read_.load(std::memory_order_acquire) == records_.size()
        || tag.size() + message.size() + 4 > Record{}.bytes.size()) return false;
    auto& record = records_[next % records_.size()];
    char* out = record.bytes.data();
    *out++ = '['; if (!tag.empty()) std::memcpy(out, tag.data(), tag.size()); out += tag.size();
    *out++ = ']'; *out++ = ' ';
    if (!message.empty()) std::memcpy(out, message.data(), message.size()); out += message.size(); *out++ = '\n';
    record.size = out - record.bytes.data();
    written_.store(next + 1, std::memory_order_release);
    return true;
  }
  bool pop(Record& record) noexcept {
    const auto next = read_.load(std::memory_order_relaxed);
    if (next == written_.load(std::memory_order_acquire)) return false;
    const auto& source = records_[next % records_.size()];
    record.size = source.size;
    std::memcpy(record.bytes.data(), source.bytes.data(), record.size);
    read_.store(next + 1, std::memory_order_release);
    return true;
  }
 private:
  std::array<Record, Capacity> records_;
  std::atomic<std::size_t> written_{0}, read_{0};
};
using DiagnosticBuffer = SizedDiagnosticBuffer<64>;
}
