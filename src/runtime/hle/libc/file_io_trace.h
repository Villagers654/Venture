#pragma once
#include <cstdint>
#include <string_view>

namespace venture::hle::libc::file_io_trace {
// VENTURE_TRACE_FILE_IO=1: guest file opens, reads, closes and file mappings.
// Failed and slow calls are logged as they happen; the most recent calls stay
// in a bounded ring that shutdown snapshots print with the open descriptors.
extern const bool enabled;
std::int64_t start();
void opened(std::string_view path, int flags, std::int64_t result);
void read(char op, int fd, std::string_view path, std::uint64_t n, std::int64_t offset,
          std::int64_t result, std::int64_t start_ns);
void closed(int fd, std::int64_t result);
void mapped(int fd, std::string_view path, std::uint64_t size, std::int64_t offset, std::int64_t result);
}  // namespace venture::hle::libc::file_io_trace
