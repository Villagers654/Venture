#pragma once
#include "hle/vrapi/abi.h"
namespace venture::hle::vrapi::integration {
// Vader 1.1.1 libvrintegrationloader.so Initialize @0xb1c reads version
// words at 0/4/8/12, JNIEnv at 0x18. Its OVRPlugin @0x3ab88 supplies
// {1,1,16,0,ovrJava}; SetThreadPerformance @0x3ecb0 has (tid,perf),
// AutoThreadScheduling @0x3ed10 has (count,tids,perf,mode).
struct Init {
  std::int32_t product, major, minor, patch;
  abi::Java java;
};
std::int32_t initialize(const Init *);
void shutdown();
bool has_initialized();
std::int32_t clear_state(std::uint32_t);
std::int32_t set_thread_performance(std::uint32_t, std::int32_t);
std::int32_t auto_thread_scheduling(std::uint32_t, const std::uint32_t *, const std::int32_t *,
                                    std::int32_t);
std::int32_t usable_cores();
} // namespace venture::hle::vrapi::integration
