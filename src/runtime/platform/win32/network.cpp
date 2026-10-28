#include "platform/network.h"
#include "platform/change_cache.h"
#include "platform/win32/network_watch.h"

// IP Helper declarations require Winsock's address types before these headers.
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <netlistmgr.h>
// clang-format on

#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace venture::platform {
namespace {
// NL_NETWORK_CONNECTIVITY_HINT (netioapi.h, Windows 10 2004); not declared by
// the MinGW headers, so the export is resolved at run time.
struct ConnectivityHint {
  std::int32_t level, cost;
  BOOLEAN approaching_data_limit, over_data_limit, roaming;
};
static_assert(sizeof(ConnectivityHint) == 12);
using ConnectivityHintQuery = DWORD(WINAPI*)(ConnectivityHint*);
constexpr std::int32_t kInternetAccess = 3; // NetworkConnectivityLevelHintInternetAccess
ConnectivityHintQuery connectivity_hint_query() {
  static const auto query = [] {
    const auto module = GetModuleHandleW(L"iphlpapi.dll");
    return module ? reinterpret_cast<ConnectivityHintQuery>(
                        reinterpret_cast<void*>(GetProcAddress(module, "GetNetworkConnectivityHint")))
                  : nullptr;
  }();
  return query;
}
bool nlm_internet_connectivity() {
  const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
    throw std::runtime_error("Host network validation initialization failed");
  struct Apartment {
    bool owned;
    ~Apartment() { if (owned) CoUninitialize(); }
  } apartment{SUCCEEDED(initialized)};
  INetworkListManager* raw = nullptr;
  if (FAILED(CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL,
                              IID_INetworkListManager, reinterpret_cast<void**>(&raw))))
    throw std::runtime_error("Host network validation service unavailable");
  auto release = [](INetworkListManager* manager) { manager->Release(); };
  std::unique_ptr<INetworkListManager, decltype(release)> manager(raw, release);
  NLM_CONNECTIVITY connectivity = NLM_CONNECTIVITY_DISCONNECTED;
  if (FAILED(manager->GetConnectivity(&connectivity)))
    throw std::runtime_error("Host network validation query failed");
  return (connectivity & (NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_INTERNET)) != 0;
}

struct NetworkWatcher {
  void (*changed)(void*);
  void* context;
  void notify() const { changed(context); }
  static void WINAPI route_changed(void* watcher, MIB_IPFORWARD_ROW2*, MIB_NOTIFICATION_TYPE) {
    static_cast<NetworkWatcher*>(watcher)->notify();
  }
  static void WINAPI interface_changed(void* watcher, MIB_IPINTERFACE_ROW*, MIB_NOTIFICATION_TYPE) {
    static_cast<NetworkWatcher*>(watcher)->notify();
  }
  static void WINAPI address_changed(void* watcher, MIB_UNICASTIPADDRESS_ROW*, MIB_NOTIFICATION_TYPE) {
    static_cast<NetworkWatcher*>(watcher)->notify();
  }
};

std::int32_t query_active_network_type() {
  MIB_IPFORWARD_TABLE2* raw = nullptr;
  if (GetIpForwardTable2(AF_UNSPEC, &raw) != NO_ERROR)
    throw std::runtime_error("Host network route query failed");
  std::unique_ptr<MIB_IPFORWARD_TABLE2, decltype(&FreeMibTable)> routes(raw, FreeMibTable);
  ULONG size = 16384, status = 0;
  std::vector<unsigned char> buffer(size);
  for (int attempt = 0; attempt < 3; ++attempt) {
    status = GetAdaptersAddresses(
        AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    if (status != ERROR_BUFFER_OVERFLOW)
      break;
    buffer.resize(size);
  }
  if (status == ERROR_NO_DATA)
    return -2;
  if (status != NO_ERROR)
    throw std::runtime_error("Host network adapter query failed");
  std::uint64_t best = std::numeric_limits<std::uint64_t>::max();
  std::int32_t type = -2;
  for (ULONG i = 0; i < routes->NumEntries; ++i) {
    const auto& route = routes->Table[i];
    if (route.DestinationPrefix.PrefixLength != 0 || route.ValidLifetime == 0)
      continue;
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter;
         adapter = adapter->Next) {
      if (adapter->Luid.Value != route.InterfaceLuid.Value ||
          adapter->OperStatus != IfOperStatusUp || !adapter->FirstUnicastAddress ||
          adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
        continue;
      const auto metric =
          std::uint64_t(route.Metric) + (route.DestinationPrefix.Prefix.si_family == AF_INET
                                             ? adapter->Ipv4Metric
                                             : adapter->Ipv6Metric);
      if (metric >= best)
        continue;
      best = metric;
      // ConnectivityManager.java: MOBILE=0, WIFI=1, ETHERNET=9, VPN=17.
      switch (adapter->IfType) {
      case IF_TYPE_IEEE80211:
        type = 1;
        break;
      case IF_TYPE_ETHERNET_CSMACD:
        type = 9;
        break;
      case IF_TYPE_WWANPP:
      case IF_TYPE_WWANPP2:
        type = 0;
        break;
      case IF_TYPE_TUNNEL:
        type = 17;
        break;
      default:
        type = -1;
        break;
      }
    }
  }
  return type;
}
} // namespace

bool has_internet_connectivity() {
  // Windows' Internet assessment (NCSI), rather than treating a default route
  // or an up interface as validated Internet access. The hint is a cheap local
  // query; creating the NLM COM object costs milliseconds per call.
  if (const auto query = connectivity_hint_query()) {
    ConnectivityHint hint{};
    if (query(&hint) != NO_ERROR)
      throw std::runtime_error("Host network validation query failed");
    return hint.level == kInternetAccess;
  }
  return nlm_internet_connectivity();
}

bool watch_network_changes(void (*changed)(void*), void* context, const RouteNotifications& api) {
  // Callbacks may arrive until exit, and a cancelled registration may still be
  // finishing one, so the watcher is never freed.
  auto* watcher = new NetworkWatcher{changed, context};
  HANDLE route = nullptr, iface = nullptr, address = nullptr;
  const bool watched =
      api.route(AF_UNSPEC, &NetworkWatcher::route_changed, watcher, FALSE, &route) == NO_ERROR &&
      api.iface(AF_UNSPEC, &NetworkWatcher::interface_changed, watcher, FALSE, &iface) == NO_ERROR &&
      api.address(AF_UNSPEC, &NetworkWatcher::address_changed, watcher, FALSE, &address) == NO_ERROR;
  if (watched) return true;
  for (auto handle : {route, iface, address})
    if (handle) api.cancel(handle);
  return false;
}

// Android's ConnectivityManager answers from system service state. Route and
// adapter tables take milliseconds to read, so the last answer is kept until
// Windows reports a route, interface or unicast address change.
std::int32_t active_network_type() {
  // Notification callbacks may arrive until process exit.
  static auto* const type = new WatchedQuery<std::int32_t>(
      [](auto changed, void* context) { return watch_network_changes(changed, context); },
      query_active_network_type);
  return type->get();
}
} // namespace venture::platform
