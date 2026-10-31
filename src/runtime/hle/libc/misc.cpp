#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <vector>

#include "common/log.h"
#include "core/libc_lifecycle.h"
#include "core/process.h"
#include "core/shutdown_trace.h"
#include "core/guest_storage.h"
#include "core/signals.h"
#include "hle/libc/libc_a.h"
#include "abi32.h"
#include "platform/libc_host.h"
namespace venture::hle::libc {
int* error_location() {
  if (auto t = GuestThread::current()) return t->errno_location();
  if (process_config().isa() == GuestIsa::a32) {
    thread_local GuestStorage cell;
    return g2h<int>(cell.resize(sizeof(int)));
  }
  thread_local int host_hle_errno = 0;
  return &host_hle_errno;
}
void set_error(int value) { *error_location() = value; }
[[noreturn]] void fatal(std::string_view message) {
  log::error("libc", "{}", message);
  if (auto t = GuestThread::current()) {
    signals::Info info{};
    info.signo = signals::kSigAbrt;
    signals::deliver_synchronous(*t, info);
    // abort is non-returning even if an installed SIGABRT handler returns.
    signals::Action action{};
    signals::set_action(signals::kSigAbrt, &action, nullptr);
    signals::deliver_synchronous(*t, info);
  }
  // Only reachable without an attached CPU or with a broken core crash handler.
  throw std::runtime_error("guest fatal error: " + std::string(message));
}
namespace {
struct Property {
  std::string name, value;
  std::uint32_t serial;
};
const std::map<std::string, Property>& properties() {
  static const auto all = [] {
    const auto& device = *process_config().device_identity;
    std::map<std::string, std::string> defaults = {
        {"ro.product.manufacturer", "Oculus"},
        {"ro.product.model", std::string(device.model)},
        {"ro.product.brand", "oculus"},
        {"ro.product.device", std::string(device.device)},
        {"ro.product.name", std::string(device.device)},
        {"ro.product.board", std::string(device.device)},
        {"ro.hardware", std::string(device.device)},
        {"ro.build.version.sdk", std::to_string(process_config().sdk_level)},
        {"ro.build.version.release", "12"},
        {"ro.build.version.codename", "REL"},
        {"ro.debuggable", "0"},
        {"ro.secure", "1"},
        {"ro.product.cpu.abi", process_config().isa() == GuestIsa::a32 ? "armeabi-v7a" : "arm64-v8a"},
        {"ro.product.cpu.abilist", process_config().isa() == GuestIsa::a32 ? "armeabi-v7a,armeabi" : "arm64-v8a"},
        // Runtime identity, not a copied hardware attestation fingerprint. Owners
        // may supply the installed firmware's value through ProcessConfig.
        {"ro.build.fingerprint", "oculus/" + std::string(device.device) + "/" +
                                     std::string(device.device) + ":12/Venture/0:user/release-keys"}};
    for (auto partition : {"system", "system_ext", "product", "vendor", "odm", "bootimage"})
      for (auto field : {"manufacturer", "model", "brand", "device", "name"})
        defaults["ro.product." + std::string(partition) + "." + field] =
            defaults.at("ro.product." + std::string(field));
    // No unverified ro.board.platform guess, and no ro.kernel.qemu property.
    for (const auto& [name, value] : process_config().properties) defaults[name] = value;
    std::map<std::string, Property> result;
    for (auto& [name, value] : defaults) {
      // PROP_VALUE_MAX=92 for the legacy APIs; read_callback supports long ro.*.
      auto serial = static_cast<std::uint32_t>(std::min<std::size_t>(255, value.size())) << 24;
      result.emplace(name, Property{name, std::move(value), serial});
    }
    return result;
  }();
  return all;
}
const Property* property_find(const char* name) {
  auto& all = properties();
  auto p = all.find(name);
  return p == all.end() ? nullptr : &p->second;
}
int property_get(const char* name, char* value) {
  auto p = property_find(name);
  if (!p) {
    value[0] = 0;
    return 0;
  }
  auto n = std::min<std::size_t>(91, p->value.size());
  std::memcpy(value, p->value.data(), n);
  value[n] = 0;
  return static_cast<int>(n);
}
int property_read(const Property* p, char* name, char* value) {
  if (name) {
    auto n = std::min<std::size_t>(31, p->name.size());
    std::memcpy(name, p->name.data(), n);
    name[n] = 0;
  }
  return property_get(p->name.c_str(), value);
}
void property_callback(CallContext& c, const Property* p, GuestAddr callback, GuestAddr cookie) {
  c.thread().call(callback, {cookie, h2g(p->name.c_str()), h2g(p->value.c_str()), p->serial});
}
int property_foreach(CallContext& c, GuestAddr callback, GuestAddr cookie) {
  for (const auto& [_, p] : properties()) c.thread().call(callback, {h2g(&p), cookie});
  return 0;
}
std::uint32_t property_serial(const Property* p) { return p->serial; }
int api_level() {
  auto p = property_find("ro.build.version.sdk");
  return p ? static_cast<int>(parse_signed(p->value.c_str(), nullptr, 10)) : process_config().sdk_level;
}
std::uint64_t stack_guard = 0;
void stack_fail() { fatal("stack protector: guest stack corruption"); }
void assert_impl(const char* file, int line, const char* expression) {
  fatal(std::string(file) + ":" + std::to_string(line) + ": assertion failed: " + expression);
}
void assert2_impl(const char* file, int line, const char* function, const char* expression) {
  fatal(std::string(file) + ":" + std::to_string(line) + ": " + function +
        ": assertion failed: " + expression);
}
std::mutex abort_mutex;
std::string abort_message;
void abort_message_impl(const char* text) {
  std::lock_guard lock(abort_mutex);
  abort_message = text ? text : "";
}
std::atomic<const char*> program_name{"venture"};
const char* getprogname_impl() { return program_name.load(); }
void setprogname_impl(const char* name) {
  auto base = std::strrchr(name, '/');
  program_name.store(base ? base + 1 : name);
}
int sigrtmin() { return 41; }  // API 32 platform/bionic/reserved_signals.h reserves 32..40.
int sigrtmax() { return 64; }
struct Destructor {
  GuestAddr function, arg, dso;
  bool no_arg = false;
};
std::mutex exit_mutex;
std::vector<Destructor> exit_handlers;
thread_local std::vector<Destructor> thread_handlers;
int cxa_atexit_impl(GuestAddr f, GuestAddr arg, GuestAddr dso) {
  shutdown_trace::trace_destructor("__cxa_atexit register", f, dso);
  std::lock_guard lock(exit_mutex);
  try {
    exit_handlers.push_back({f, arg, dso});
    return 0;
  } catch (const std::bad_alloc&) {
    set_error(err::kENOMEM);
    return -1;
  }
}
// ARM C++ ABI 3.3.5.3: __cxa_atexit with the object first (bionic arm only).
int aeabi_atexit_impl(GuestAddr object, GuestAddr f, GuestAddr dso) {
  return cxa_atexit_impl(f, object, dso);
}
int atexit_impl(GuestAddr f) {
  shutdown_trace::trace_destructor("atexit register", f, 0);
  std::lock_guard lock(exit_mutex);
  try {
    exit_handlers.push_back({f, 0, 0, true});
    return 0;
  } catch (const std::bad_alloc&) {
    set_error(err::kENOMEM);
    return -1;
  }
}
void finalize(GuestThread& thread, GuestAddr dso) {
  for (;;) {
    Destructor d{};
    {
      std::lock_guard lock(exit_mutex);
      auto i = std::find_if(exit_handlers.rbegin(), exit_handlers.rend(),
                            [&](const auto& h) { return !dso || h.dso == dso; });
      if (i == exit_handlers.rend()) break;
      d = *i;
      exit_handlers.erase(std::next(i).base());
    }
    shutdown_trace::trace_destructor(dso ? "__cxa_finalize invoke" : "process exit invoke", d.function, d.dso);
    if (d.no_arg)
      thread.call(d.function, {});
    else
      thread.call(d.function, {d.arg});
  }
}
void cxa_finalize_impl(CallContext& c, GuestAddr dso) { finalize(c.thread(), dso); }
int cxa_thread_atexit_impl(GuestAddr f, GuestAddr arg, GuestAddr dso) {
  shutdown_trace::trace_destructor("__cxa_thread_atexit register", f, dso);
  try {
    thread_handlers.push_back({f, arg, dso});
    return 0;
  } catch (const std::bad_alloc&) {
    set_error(err::kENOMEM);
    return -1;
  }
}
struct AtFork {
  GuestAddr prepare, parent, child, dso;
};
std::vector<AtFork> fork_handlers;
void register_atfork_impl(GuestAddr prepare, GuestAddr parent, GuestAddr child, GuestAddr dso) {
  std::lock_guard lock(exit_mutex);
  fork_handlers.push_back({prepare, parent, child, dso});
}
}  // namespace
std::string_view system_property(std::string_view name) {
  const auto& all = properties();
  auto found = all.find(std::string(name));
  return found == all.end() ? std::string_view{} : found->second.value;
}
void register_misc(HleLibrary& lib) {
  if (process_config().isa() == GuestIsa::a32) {
    abi32::adapt<&cxa_atexit_impl>("__cxa_atexit");
    abi32::adapt<&aeabi_atexit_impl>("__aeabi_atexit");
    abi32::adapt<&cxa_thread_atexit_impl>("__cxa_thread_atexit");
    abi32::adapt<&cxa_thread_atexit_impl>("__cxa_thread_atexit_impl");
    abi32::adapt<&atexit_impl>("atexit");
    abi32::adapt<&register_atfork_impl>("__register_atfork");
  }
  extern void register_options(HleLibrary&);
  register_options(lib);
  if (!platform::secure_random(&stack_guard, sizeof(stack_guard))) fatal("cannot initialize stack protector");
  stack_guard &= ~0xffull;
  // The core registers these identity-addressed HLE data objects as non-executable.
  if (process_config().isa() == GuestIsa::a32) {
    auto guard32 = static_cast<std::uint32_t>(stack_guard);
    lib.add_data("__stack_chk_guard", low_data(std::as_bytes(std::span(&guard32, 1))));
  } else
    lib.add_data("__stack_chk_guard", h2g(&stack_guard));
#define B(n, f) bind<&f>(lib, n)
  B("__system_property_get", property_get);
  B("__system_property_find", property_find);
  B("__system_property_read", property_read);
  B("__system_property_read_callback", property_callback);
  B("__system_property_foreach", property_foreach);
  B("__system_property_serial", property_serial);
  B("android_get_device_api_level", api_level);
  B("__android_get_device_api_level", api_level);
  B("__stack_chk_fail", stack_fail);
  B("__assert", assert_impl);
  B("__assert2", assert2_impl);
  B("android_set_abort_message", abort_message_impl);
  B("__errno", error_location);
  B("__libc_current_sigrtmin", sigrtmin);
  B("__libc_current_sigrtmax", sigrtmax);
  B("getprogname", getprogname_impl);
  B("setprogname", setprogname_impl);
  B("__cxa_atexit", cxa_atexit_impl);
  B("__cxa_finalize", cxa_finalize_impl);
  if (process_config().isa() == GuestIsa::a32) B("__aeabi_atexit", aeabi_atexit_impl);
  B("atexit", atexit_impl);
  B("__cxa_thread_atexit_impl", cxa_thread_atexit_impl);
  B("__cxa_thread_atexit", cxa_thread_atexit_impl);
  B("__register_atfork", register_atfork_impl);
#undef B
}
void run_thread_destructors(GuestThread& t) {
  while (!thread_handlers.empty()) {
    auto d = thread_handlers.back();
    thread_handlers.pop_back();
    shutdown_trace::trace_destructor("thread exit invoke", d.function, d.dso);
    t.call(d.function, {d.arg});
  }
}
void run_exit_destructors(GuestThread& t) { finalize(t, 0); }
}  // namespace venture::hle::libc
namespace venture {
void run_guest_thread_destructors(GuestThread& thread) { hle::libc::run_thread_destructors(thread); }
void run_guest_exit_handlers(GuestThread& thread) { hle::libc::run_exit_destructors(thread); }
}  // namespace venture
