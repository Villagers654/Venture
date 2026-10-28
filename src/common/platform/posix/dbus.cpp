#include "common/platform/posix/dbus.h"

#include <dlfcn.h>

#include <mutex>
#include <type_traits>

namespace venture::dbus {
namespace {
// Declarations follow <systemd/sd-bus.h>; buses and messages stay opaque.
struct Error {
  const char* name = nullptr;
  const char* message = nullptr;
  int need_free = 0;
};
struct Library {
  int (*open_system)(void**);
  int (*open_user)(void**);
  int (*get_property_trivial)(void*, const char*, const char*, const char*, const char*, Error*, char, void*);
  int (*call_method)(void*, const char*, const char*, const char*, const char*, Error*, void**, const char*, ...);
  int (*message_read)(void*, const char*, ...);
  void* (*message_unref)(void*);
  void (*error_free)(Error*);
  void* (*flush_close_unref)(void*);
  bool loaded = false;
  Library() {
    void* library = dlopen("libsystemd.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!library) return;
    bool ok = true;
    auto bind = [&](auto& function, const char* name) {
      function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(dlsym(library, name));
      ok = ok && function;
    };
    bind(open_system, "sd_bus_open_system");
    bind(open_user, "sd_bus_open_user");
    bind(get_property_trivial, "sd_bus_get_property_trivial");
    bind(call_method, "sd_bus_call_method");
    bind(message_read, "sd_bus_message_read");
    bind(message_unref, "sd_bus_message_unref");
    bind(error_free, "sd_bus_error_free");
    bind(flush_close_unref, "sd_bus_flush_close_unref");
    loaded = ok;
  }
};
const Library& library() {
  static const auto* const value = new Library;  // used until exit
  return *value;
}
struct Connection {
  Bus kind;
  std::mutex mutex;  // an sd_bus connection is not thread-safe
  void* bus = nullptr;
  explicit Connection(Bus k) : kind(k) {}
  // Runs `f(bus, error)` on a connected bus; a transport failure drops the
  // connection so the next call reconnects.
  template <class F> int run(F&& f) {
    const auto& l = library();
    if (!l.loaded) return -1;
    std::lock_guard lock(mutex);
    if (!bus && (kind == Bus::system ? l.open_system(&bus) : l.open_user(&bus)) < 0) {
      bus = nullptr;
      return -1;
    }
    Error error;
    const int r = f(bus, &error);
    const bool remote = error.name != nullptr;  // the peer replied with an error; the bus is fine
    l.error_free(&error);
    if (r < 0 && !remote) {
      l.flush_close_unref(bus);
      bus = nullptr;
    }
    return r;
  }
};
Connection& connection(Bus bus) {
  static auto* const system = new Connection(Bus::system);  // used until exit
  static auto* const session = new Connection(Bus::session);
  return bus == Bus::system ? *system : *session;
}
template <class T>
std::optional<T> property(Bus bus, const char* destination, const char* path, const char* interface,
                          const char* member, char type) {
  T value{};
  const int r = connection(bus).run([&](void* b, Error* error) {
    return library().get_property_trivial(b, destination, path, interface, member, error, type, &value);
  });
  if (r < 0) return std::nullopt;
  return value;
}
// Calls a method and discards its reply.
template <class... Args>
bool call(Bus bus, const char* destination, const char* path, const char* interface, const char* member,
          const char* signature, Args... args) {
  return connection(bus).run([&](void* b, Error* error) {
    return library().call_method(b, destination, path, interface, member, error, nullptr, signature, args...);
  }) >= 0;
}
constexpr const char* portal = "org.freedesktop.portal.Desktop";
constexpr const char* portal_path = "/org/freedesktop/portal/desktop";
}  // namespace

std::optional<std::uint32_t> u32_property(Bus bus, const char* destination, const char* path, const char* interface,
                                          const char* member) {
  return property<std::uint32_t>(bus, destination, path, interface, member, 'u');
}
std::optional<std::int32_t> i32_property(Bus bus, const char* destination, const char* path, const char* interface,
                                         const char* member) {
  return property<std::int32_t>(bus, destination, path, interface, member, 'i');
}
bool call_ti(Bus bus, const char* destination, const char* path, const char* interface, const char* member,
             std::uint64_t t, std::int32_t i) {
  return call(bus, destination, path, interface, member, "ti", t, i);
}
std::optional<std::uint32_t> portal_connectivity() {
  std::uint32_t value = 0;
  const int r = connection(Bus::session).run([&](void* b, Error* error) {
    void* reply = nullptr;
    int result = library().call_method(b, portal, portal_path, "org.freedesktop.portal.NetworkMonitor",
                                       "GetConnectivity", error, &reply, "");
    if (result >= 0) result = library().message_read(reply, "u", &value);
    if (reply) library().message_unref(reply);
    return result;
  });
  if (r < 0) return std::nullopt;
  return value;
}
bool portal_open_uri(const std::string& uri) {
  // Parent window "" (none) and no options.
  return call(Bus::session, portal, portal_path, "org.freedesktop.portal.OpenURI", "OpenURI", "ssa{sv}", "",
              uri.c_str(), 0u);
}
bool portal_open_file(int fd) {
  return call(Bus::session, portal, portal_path, "org.freedesktop.portal.OpenURI", "OpenFile", "sha{sv}", "", fd, 0u);
}
std::optional<std::int32_t> portal_min_nice() {
  return i32_property(Bus::session, portal, portal_path, "org.freedesktop.portal.Realtime", "MinNiceLevel");
}
bool portal_thread_high_priority(std::uint64_t pid, std::uint64_t tid, std::int32_t nice) {
  return call(Bus::session, portal, portal_path, "org.freedesktop.portal.Realtime", "MakeThreadHighPriorityWithPID",
              "tti", pid, tid, nice);
}
}  // namespace venture::dbus
