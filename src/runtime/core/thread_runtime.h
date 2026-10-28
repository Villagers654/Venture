#pragma once
#include <cstdint>
#include <exception>
#include <functional>
#include <vector>
#include "core/thread.h"
#include "core/registry_lock.h"

namespace venture {
// pthread_t is this core-owned opaque guest address; no bionic pthread_internal_t ABI is exposed.
struct GuestThreadOptions {
  GuestAddr stack = 0;
  std::uint64_t stack_size = 1024 * 1024;
  std::uint64_t guard_size = kGuestPageSize;
  bool detached = false;
  void (*on_exit)(GuestThread&) = nullptr;
};
struct ThreadExit final : std::exception {
  std::uint64_t value;
  explicit ThreadExit(std::uint64_t v) : value(v) {}
  const char* what() const noexcept override { return "guest thread exited"; }
};
struct ProcessExit final : std::exception {
  int status;
  explicit ProcessExit(int s) : status(s) {}
  const char* what() const noexcept override { return "guest process exited"; }
};
// An aliasing shared_ptr pins even a detached thread until pthread_create has copied its handle.
std::shared_ptr<GuestThread> create_guest_thread(GuestAddr start, GuestAddr argument, const GuestThreadOptions& options = {});
int join_guest_thread(GuestAddr pthread, std::uint64_t* result);
int detach_guest_thread(GuestAddr pthread);
[[noreturn]] void exit_guest_thread(std::uint64_t result);
[[noreturn]] void exit_guest_process(int status);
// A dedicated app worker is the guest process: terminal exit must not unwind
// into unrelated host/JVM shutdown. Embedded fixtures retain exception control.
void set_terminal_guest_process_exit(bool enabled);
bool terminal_guest_process_exit();
// Fatal default-action signals and enforced quit interruption cannot be erased
// by an earlier/later guest exit(0). Normal explicit status retains first-wins.
void record_guest_process_failure(int status);
// Preserve a fatal outcome without interrupting the crash reporter that is
// about to run. Any subsequent process exit/terminal flush publishes it.
void record_guest_process_failure_on_exit(int status);
void interrupt_guest_process_after_grace();
// Host quit exhausted its backstop. Dedicated workers terminate without guest
// pthread/TLS/allocator teardown; embedded users retain exception control.
[[noreturn]] void end_guest_process_after_grace();
// -1 while running, otherwise the effective process outcome (including failure).
// Lets a Java main-loop owner observe exit() from a native worker.
int guest_process_exit_status();
// Includes a deferred fatal abort latch before the terminal flush publishes it.
int recorded_guest_process_failure();
void check_guest_exit(GuestThread& thread);
// Process-owner teardown: reject new workers, interrupt and drain existing workers.
void shutdown_guest_threads();
// Default cleanup for adopted and managed threads, before CPU/TLS teardown.
void set_guest_thread_exit_handler(void (*handler)(GuestThread&));
// Native service TLS cleanup after guest destructors, even on forced exit.
void set_guest_thread_native_cleanup(void (*handler)());
void finish_guest_thread(GuestThread& thread);
// Visitors run under the registry lock, pinning the thread's lifetime for the callback.
void for_each_guest_thread(const std::function<void(GuestThread&)>& visit);
bool try_for_each_guest_thread(const std::function<void(GuestThread&)>& visit);
RegistryMutex::Owner thread_registry_owner();
struct ManagedThreadSnapshot {
  std::int32_t tid;
  GuestAddr pthread, done_word;
  std::uint32_t done;
  bool detached, joining, live;
};
std::vector<ManagedThreadSnapshot> managed_thread_snapshot();
void interrupt_waits(GuestThread& thread);
}
