#pragma once
#include <cstdint>
#include <optional>
#include <string>

namespace venture::dbus {
// D-Bus through libsystemd's sd-bus, loaded at run time. One connection per
// bus serves the process; it is reopened after a failure, so a restarted bus
// or service is picked up. Every call fails (nullopt/false) without
// libsystemd or a reachable bus.
enum class Bus { system, session };
std::optional<std::uint32_t> u32_property(Bus bus, const char* destination, const char* path, const char* interface,
                                          const char* member);
std::optional<std::int32_t> i32_property(Bus bus, const char* destination, const char* path, const char* interface,
                                         const char* member);
// A method taking (uint64, int32) and returning nothing of interest, such as
// RealtimeKit's MakeThreadHighPriority(thread, nice).
bool call_ti(Bus bus, const char* destination, const char* path, const char* interface, const char* member,
             std::uint64_t t, std::int32_t i);

// XDG desktop portals on the session bus, which sandboxed and unsandboxed
// processes share.
// NetworkMonitor.GetConnectivity: 1 local only, 2 limited, 3 captive portal, 4 full.
std::optional<std::uint32_t> portal_connectivity();
// OpenURI.OpenURI and OpenURI.OpenFile (a file or directory descriptor, which
// the portal duplicates). True once a portal accepted the request.
bool portal_open_uri(const std::string& uri);
bool portal_open_file(int fd);
// Realtime.MakeThreadHighPriorityWithPID and its MinNiceLevel property.
std::optional<std::int32_t> portal_min_nice();
bool portal_thread_high_priority(std::uint64_t pid, std::uint64_t tid, std::int32_t nice);
}  // namespace venture::dbus
