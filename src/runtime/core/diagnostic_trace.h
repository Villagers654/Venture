#pragma once
#include <string_view>
namespace venture {
// Initialize on host startup, before guest waits. Producers never initialize a
// sink, acquire the writer lock, compress, or write files; overflow drops records.
void initialize_diagnostic_trace();
void diagnostic_trace(std::string_view tag, std::string_view message) noexcept;
void flush_diagnostic_trace();
}
