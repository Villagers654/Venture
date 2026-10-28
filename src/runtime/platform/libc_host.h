#pragma once
#include <cstdint>
#include <ctime>
#include <string>
namespace venture::platform {
bool secure_random(void* out, std::uint64_t size);
void close_host_fd(int fd);
int libc_host_error();  // capture/translate current CRT errno to a Linux value
bool local_time(std::int64_t seconds, std::tm& result);
bool make_local_time(std::tm& value, std::int64_t& seconds);
struct TimeZone {
  std::int64_t seconds_west;
  bool has_dst;
  std::string standard, daylight;
};
TimeZone time_zone();
}  // namespace venture::platform
