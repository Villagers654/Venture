#pragma once
#include "core/av_telemetry.h"
#include "core/wait.h"
#include <string>
namespace venture::av {
inline std::int64_t now_ns() { return monotonic_ns(); }
// Compatibility facade for non-realtime audio producers; both APIs use the same writer.
class Telemetry {
 public:
  static Telemetry &get() { static Telemetry t; return t; }
  bool enabled() const { return venture::av::enabled(); }
};
inline bool emit(std::string kind, std::string fields) {
  if (!enabled()) return false;
  try {
    auto event = nlohmann::json::parse("{" + fields + "}");
    event["kind"] = std::move(kind);
    emit(std::move(event));
    return true;
  } catch (...) { return false; }
}
}
