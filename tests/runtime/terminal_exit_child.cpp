#include "core/signals.h"
#include "core/thread_runtime.h"
#include "core/process_exit.h"
#include "core/profile.h"
#include "core/invalidation_stats.h"
#include "common/paths.h"
#include "hle/vulkan/vulkan.h"
#include "common/log.h"
#include <chrono>
#include <string_view>
#include <thread>
#include <cstdio>
#include <string>
#include <cstdlib>
#include <fstream>

int main(int argc, char** argv) {
  using namespace venture;
  const std::string_view mode = argc > 1 ? argv[1] : "";
  if (mode == "diagnostic-terminal" || mode == "diagnostic-zero" || mode == "diagnostic-return") {
    if (!profile::enabled || !profile::invalidation_tracing) return 97;
    const auto* marker = std::getenv("VENTURE_TERMINAL_TEST_ATEXIT");
    const auto* logfile = std::getenv("VENTURE_TERMINAL_TEST_LOG");
    if (!marker || !logfile) return 96;
    log::set_file(logfile);
    log::set_repeat_period(std::chrono::milliseconds(0));
    // A normal return runs this; the real terminal process path must not.
    std::atexit([] {
      std::ofstream out(paths::from_utf8(std::getenv("VENTURE_TERMINAL_TEST_ATEXIT")));
      out << "atexit ran\n";
    });
    profile::start();
    profile::mark("terminal_diagnostic_fixture");
    profile::record_invalidation(InvalidationCause::map, 4096, 1, 0, monotonic_ns(), 1000);
    if (mode == "diagnostic-return") {
      flush_guest_process_files(0);
      flush_guest_process_files(0);
      return 0;
    }
    set_terminal_guest_process_exit(true);
    exit_guest_process(mode == "diagnostic-zero" ? 0 : 73);
  }
  const bool retain = mode == "retain";
  if (mode == "grace-after-zero" || mode == "signal-after-zero") {
    try { exit_guest_process(0); } catch (const ProcessExit&) {}
    if (mode == "grace-after-zero") interrupt_guest_process_after_grace();
    else {
      set_terminal_guest_process_exit(true);
      GuestThread thread(777); thread.attach_to_current_host_thread();
      signals::deliver_synchronous(thread, {signals::kSigSegv, 1, 0});
      return 99;
    }
  }
  if (mode == "device-lost" || mode == "device-lost-host") {
    set_terminal_guest_process_exit(true);
    GuestThread thread(777);
    if (mode == "device-lost") thread.attach_to_current_host_thread();
    vulkan::end_app_after_device_loss();
    if (mode == "device-lost") return 99;
    exit_guest_process(37);
  }
  if (retain) {
    try { exit_guest_process(23); } catch (const ProcessExit&) {}
  }
  set_terminal_guest_process_exit(true);
  if (mode == "blocked-logger") {
    // Parent stops reading stderr once this write has entered the logger.
    // The large pipe write then holds logging/stdio locks across terminal exit.
    std::thread([] { log::error("test", "{}", std::string(1024 * 1024, 'x')); }).detach();
    if (std::getchar() != 'G') return 98;
    exit_guest_process(37);
  }
  // Models an unrelated VM/service thread which has no reason to terminate.
  std::thread([] { std::this_thread::sleep_for(std::chrono::hours(1)); }).detach();
  struct UnrelatedCleanup {
    ~UnrelatedCleanup() { log::error("test", "unrelated cleanup entered"); }
  } cleanup;
  exit_guest_process(retain || mode == "grace-after-zero" ? 0 : 37);
}
