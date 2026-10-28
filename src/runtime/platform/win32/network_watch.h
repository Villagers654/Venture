#pragma once
// IP Helper declarations require Winsock's address types before these headers.
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
// clang-format on

namespace venture::platform {
// IP Helper change-notification registration, injectable for tests.
struct RouteNotifications {
  decltype(&NotifyRouteChange2) route = NotifyRouteChange2;
  decltype(&NotifyIpInterfaceChange) iface = NotifyIpInterfaceChange;
  decltype(&NotifyUnicastIpAddressChange) address = NotifyUnicastIpAddressChange;
  decltype(&CancelMibChangeNotify2) cancel = CancelMibChangeNotify2;
};
// Calls changed(context) on every route, interface or unicast address change.
// Either all three registrations stay live (true) or none does (false).
bool watch_network_changes(void (*changed)(void*), void* context, const RouteNotifications& api = {});
} // namespace venture::platform
