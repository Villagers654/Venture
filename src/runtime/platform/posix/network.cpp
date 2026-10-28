#include "platform/network.h"
#include "platform/change_cache.h"
#include "platform/posix/network_linux.h"
#include "common/platform/posix/dbus.h"

#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#endif

#include <cstdlib>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace venture::platform {
namespace {
std::string read_line(const std::filesystem::path& path) {
  std::ifstream in(path);
  std::string line;
  std::getline(in, line);
  return line;
}
// Interface type from sysfs, the kernel's own classification.
std::int32_t interface_type(const std::filesystem::path& sys, const std::string& name) {
  const auto dir = sys / name;
  std::error_code ec;
  // ConnectivityManager.java: MOBILE=0, WIFI=1, ETHERNET=9, VPN=17.
  std::ifstream uevent(dir / "uevent");
  for (std::string line; std::getline(uevent, line);) {
    if (line == "DEVTYPE=wlan") return 1;
    if (line == "DEVTYPE=wwan") return 0;
    if (line == "DEVTYPE=wireguard") return 17;
  }
  if (std::filesystem::exists(dir / "wireless", ec) || std::filesystem::exists(dir / "phy80211", ec)) return 1;
  const auto type = std::strtol(read_line(dir / "type").c_str(), nullptr, 10);
  // if_arp.h: ARPHRD_ETHER 1 on a physical device; tunnels (768-776) and
  // ARPHRD_NONE (tun) carry VPNs. Bridges, veths and PPP stay unknown.
  if (type == 1) return std::filesystem::exists(dir / "device", ec) ? 9 : -1;
  if ((type >= 768 && type <= 776) || type == 65534) return 17;
  return -1;
}
bool interface_up(const std::filesystem::path& sys, const std::string& name) {
  const auto flags = std::strtoul(read_line(sys / name / "flags").c_str(), nullptr, 16);
  const auto state = read_line(sys / name / "operstate");
  return (flags & IFF_UP) && !(flags & IFF_LOOPBACK) && (state == "up" || state == "unknown");
}
std::int32_t fallback_network_type() {
  ifaddrs* raw = nullptr;
  if (getifaddrs(&raw) != 0)
    throw std::runtime_error("Host network interface query failed");
  std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> list(raw, freeifaddrs);
  for (auto* entry = list.get(); entry; entry = entry->ifa_next) {
    if (!entry->ifa_addr ||
        (entry->ifa_addr->sa_family != AF_INET && entry->ifa_addr->sa_family != AF_INET6))
      continue;
    if ((entry->ifa_flags & (IFF_UP | IFF_RUNNING | IFF_LOOPBACK)) == (IFF_UP | IFF_RUNNING))
      return -1;  // connected, transport unknown
  }
  return -2;
}
std::int32_t query_active_network_type() {
  if (auto type = default_route_network_type("/proc/net", "/sys/class/net")) return *type;
  return fallback_network_type();
}

#ifdef __linux__
// rtnetlink link, address and route multicast groups, read on a thread that
// lives until exit (notifications may).
bool watch_network_changes(void (*changed)(void*), void* context) {
  const int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
  if (fd < 0) return false;
  sockaddr_nl address{};
  address.nl_family = AF_NETLINK;
  address.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR | RTMGRP_IPV4_ROUTE |
                      RTMGRP_IPV6_ROUTE;
  if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0) {
    close(fd);
    return false;
  }
  std::thread([fd, changed, context] {
    char buffer[8192];
    for (;;) {
      const auto n = recv(fd, buffer, sizeof buffer, 0);
      // ENOBUFS: events were dropped, so the state changed in unknown ways.
      if (n != 0) changed(context);
      if (n < 0 && errno != EINTR && errno != ENOBUFS) break;
    }
    close(fd);
  }).detach();
  return true;
}
#else
bool watch_network_changes(void (*)(void*), void*) { return false; }
#endif
}  // namespace

std::optional<std::int32_t> default_route_network_type(const std::filesystem::path& proc_net,
                                                       const std::filesystem::path& sys) {
  std::ifstream v4(proc_net / "route"), v6(proc_net / "ipv6_route");
  if (!v4 && !v6) return std::nullopt;
  std::uint64_t best = std::numeric_limits<std::uint64_t>::max();
  std::int32_t type = -2;
  auto consider = [&](const std::string& name, std::uint64_t metric) {
    if (metric >= best || !interface_up(sys, name)) return;
    best = metric;
    type = interface_type(sys, name);
  };
  constexpr unsigned long route_up = 0x0001, route_reject = 0x0200;  // RTF_UP, RTF_REJECT
  std::string line;
  // Iface Destination Gateway Flags RefCnt Use Metric Mask ...
  for (std::getline(v4, line); std::getline(v4, line);) {
    std::istringstream fields(line);
    std::string name, destination, gateway, flags, refs, use, metric, mask;
    if (!(fields >> name >> destination >> gateway >> flags >> refs >> use >> metric >> mask)) continue;
    const auto f = std::stoul(flags, nullptr, 16);
    if (destination == "00000000" && mask == "00000000" && (f & route_up) && !(f & route_reject))
      consider(name, std::stoull(metric));
  }
  // destination prefix-length source source-length next-hop metric refs use flags iface
  while (std::getline(v6, line)) {
    std::istringstream fields(line);
    std::string destination, length, source, source_length, next, metric, refs, use, flags, name;
    if (!(fields >> destination >> length >> source >> source_length >> next >> metric >> refs >> use >> flags >>
          name))
      continue;
    const auto f = std::stoul(flags, nullptr, 16);
    if (destination == std::string(32, '0') && length == "00" && (f & route_up) && !(f & route_reject))
      consider(name, std::stoull(metric, nullptr, 16));
  }
  return type;
}

// The desktop's own connectivity state, the counterpart of Windows' NCSI:
// Venture sends no probes. The NetworkMonitor portal reports whichever
// monitor the session uses (NetworkManager, or netlink on systemd-networkd
// and iwd systems), also inside a sandbox; without a portal, NetworkManager
// directly. 4 is full connectivity in both. Otherwise Internet access stays
// unvalidated.
bool has_internet_connectivity() {
  if (const auto portal = dbus::portal_connectivity()) return *portal == 4;
  const auto connectivity = dbus::u32_property(dbus::Bus::system, "org.freedesktop.NetworkManager",
                                               "/org/freedesktop/NetworkManager", "org.freedesktop.NetworkManager",
                                               "Connectivity");
  return connectivity && *connectivity == 4;
}

// Route tables and sysfs take a few file reads, so the last answer is kept
// until rtnetlink reports a link, address or route change.
std::int32_t active_network_type() {
  static auto* const type = new WatchedQuery<std::int32_t>(
      [](auto changed, void* context) { return watch_network_changes(changed, context); },
      query_active_network_type);
  return type->get();
}
}  // namespace venture::platform
