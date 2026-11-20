#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace venture::platform {
// Diagnostic dumps are opt-in and confined to a copied developer run.
std::filesystem::path runtime_dump_directory(const std::filesystem::path& run_directory);
// Wait for the supervisor's bounded capture attempt before callers start
// teardown. True acknowledges completion, not necessarily a successful dump.
bool request_runtime_stall_dump(unsigned timeout_ms = 7000);
// A small launch intent must fit in one pipe write before the peer starts
// reading. Larger/Parcelable task state remains explicitly unsupported.
inline constexpr std::size_t runtime_launch_payload_limit = 4095;
enum class RuntimeExitWait : unsigned char { none, arbitration, cache_flush, profiler_flush,
                                            restart_publication, log_flush, kernel_exit,
                                            audio_capture_flush, av_flush, gpu_telemetry_flush,
                                            guest_execution_stop, java_execution_stop, save_flush,
                                            audio_shutdown, vulkan_shutdown, xr_shutdown, os_termination };
const char* runtime_exit_wait_name(RuntimeExitWait wait);
void notify_runtime_exit_wait(RuntimeExitWait wait, int status);
// Dedicated workers end through the kernel, bypassing host DLL/atexit cleanup.
[[noreturn]] void terminate_runtime_process(int status);
std::optional<int> run_runtime_dump_helper(int argc, char** argv);
struct RuntimeChildResult {
  int status; bool restart; std::string launch_payload{};
  std::filesystem::path timeout_dump{};
  unsigned dump_error = 0;
  RuntimeExitWait exit_wait = RuntimeExitWait::none;
  unsigned exit_wait_tid = 0;
  // Exit code 124 can originate in the worker's own quit backstop. Only the
  // platform supervisor knows whether it actually stopped the worker.
  bool supervisor_stopped = false;
  bool os_termination = false;
  // The worker was quitting because the host VR runtime ended its session.
  // SteamVR kills a scene app that has not exited a few seconds after it
  // ends the session; such a kill ended a quit the player asked for, not a
  // crash (quit_kill()).
  bool host_ended = false;
  // A SIGKILL of a worker that was quitting for the host runtime. On Windows
  // a TerminateProcess leaves whatever code its caller chose, which cannot be
  // told from a crash's, so nothing counts as one there.
  bool quit_kill() const { return host_ended && kill_status(status); }
  static bool kill_status(int code) {
#ifdef _WIN32
    (void)code;
    return false;
#else
    return code == 128 + 9;
#endif
  }
  // The worker's process id, for the reports it leaves behind (HotSpot's
  // fatal error report is named after it).
  unsigned long worker_pid = 0;
  // Only a successful explicit restart continues the supervisor. A stop or
  // elapsed run bound must never turn a child crash/quit timeout into success.
  std::optional<int> completion_status(bool stopping) const {
    if (!status && restart && !stopping) return std::nullopt;
    return status;
  }
};
// Dumps are opt-in for harness/dev runs and confined to that run beneath the
// LocalAppData developer evidence root. An empty/unsafe run disables dumps.

// Run one fresh app process, retaining the caller's standard streams and broker
// handles. Only our private pipe can request a restart; guest exit codes cannot.
RuntimeChildResult run_runtime_child(const std::filesystem::path& executable,
    const std::vector<std::string>& arguments, const std::vector<std::uintptr_t>& inherited,
    const std::function<bool()>& stop);
RuntimeChildResult run_runtime_child(const std::filesystem::path& executable,
    const std::vector<std::string>& arguments, const std::vector<std::uintptr_t>& inherited,
    const std::function<bool()>& stop, std::string_view launch_payload,
    bool acknowledged_teardown = false,
    const std::filesystem::path& diagnostic_run_directory = {});
// The system's own record of how a worker that ended without Venture's crash
// report crashed, waiting up to `wait` for it: on Windows, Windows Error
// Reporting's Application Error event (module, offset, status), which also
// covers a __fastfail in a host library that no in-process handler sees.
// Empty when there is none (and on other systems, whose signals Venture's
// handlers do see).
std::string system_crash_record(unsigned long pid, std::chrono::milliseconds wait);
void set_runtime_restart_channel(std::uintptr_t channel);
void set_runtime_launch_channel(std::uintptr_t channel);
void set_runtime_teardown_channel(std::uintptr_t channel);
void notify_runtime_teardown_posted();
// A file save is the worker's only remaining quit work; see RuntimeTeardownGrace::saving.
void notify_runtime_save_wait();
// The quit lifecycle reached the game (pause, stop and the final callback);
// the worker's destroy settle bound starts. See RuntimeTeardownGrace::delivered.
void notify_runtime_destroy_delivered();
// The worker quits because the host VR runtime ended its session, not because
// the launcher asked. See RuntimeChildResult::host_ended.
void notify_runtime_host_ended();
std::string take_runtime_launch_payload();
// Android Runtime.exit after a queued cold launcher intent. Kernel process
// teardown is required: native globals and HotSpot cannot be reinitialized.
[[noreturn]] void restart_runtime_process();
[[noreturn]] void restart_runtime_process(std::string_view launch_payload);
}
