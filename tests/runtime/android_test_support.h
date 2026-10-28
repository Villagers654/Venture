#pragma once
#include <functional>
#include <string>
#include "core/thread.h"
namespace venture::test {
GuestCallResult android_call(const char* lib, const char* symbol,
                             std::initializer_list<std::uint64_t> args = {},
                             std::initializer_list<double> fp = {});
GuestAddr android_callback(std::function<GuestCallResult(const GuestCallArgs&)> fn);
void android_remove_callback(GuestAddr);
bool android_has_symbol(const char* lib, const std::string& symbol);
int android_test_fd();
void android_fd_ready(int fd, std::int16_t events);
unsigned android_fd_scans(int fd);
bool android_fd_subscribed(int fd);
void android_close_fd(int fd);
std::string android_fd_path(int fd);
}  // namespace venture::test
