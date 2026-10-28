#include "platform/profile.h"
#include <algorithm>
#include <atomic>
#include "core/thread_runtime.h"
#include "core/process_exit.h"
#include "platform/runtime_child.h"
#include "platform/runtime_stop.h"
#include "platform/runtime_teardown_grace.h"
#include "common/process.h"
#include "common/paths.h"
#include "common/log.h"
#include "common/environment.h"
#include <windows.h>
#include <dbghelp.h>
#include <mutex>
#include <cstdlib>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace venture::platform {
namespace {
struct Handle {
  HANDLE value = nullptr;
  explicit Handle(HANDLE v = nullptr) : value(v) {}
  ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
};
std::uintptr_t restart_channel;
std::uintptr_t launch_channel;
std::uintptr_t teardown_channel;
[[noreturn]] void failure(const char* operation) {
  throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
}
// Dumping a live process can itself block. Isolate DbgHelp in a child and
// bound that OS wait; diagnostic work must never prevent forced termination.
int write_timeout_dump(DWORD pid, const std::filesystem::path& path) {
  const auto directory = runtime_dump_directory(path.parent_path().parent_path());
  if (directory.empty() || !paths::within(directory, path)) return ERROR_ACCESS_DENIED;
  Handle process(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid));
  if (!process.value) return GetLastError();
  auto library = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!library) return GetLastError();
  const auto write_dump = reinterpret_cast<decltype(&MiniDumpWriteDump)>(GetProcAddress(library, "MiniDumpWriteDump"));
  if (!write_dump) return GetLastError();
  Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                          CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (file.value == INVALID_HANDLE_VALUE) return GetLastError();
  const auto flags = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
  return write_dump(process.value, pid, file.value, flags, nullptr, nullptr, nullptr) ? 0 : GetLastError();
}
void timeout_dump(const std::filesystem::path& executable, DWORD pid, HANDLE job,
                  const std::filesystem::path& directory, RuntimeChildResult& result) {
  if (directory.empty()) return;
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) { result.dump_error = error.value(); return; }
  const auto path = directory / (L"worker-timeout-" + std::to_wstring(pid) + L"-" + std::to_wstring(GetTickCount64()) + L".dmp");
  std::wstring command = quote_windows_arg(executable.wstring()) + L" --runtime-dump-helper " +
      std::to_wstring(pid) + L" " + quote_windows_arg(path.wstring());
  STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
  if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
      CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
    result.dump_error = GetLastError(); return;
  }
  Handle helper(process.hProcess), thread(process.hThread);
  if (!AssignProcessToJobObject(job, helper.value) || ResumeThread(thread.value) == DWORD(-1)) {
    result.dump_error = GetLastError(); TerminateProcess(helper.value, 1); return;
  }
  const auto waited = WaitForSingleObject(helper.value, 5000);
  if (waited != WAIT_OBJECT_0) {
    result.dump_error = waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
    log::warn("launch", "Worker {} dump-helper waiter={} exceeded capture bound", pid, process.dwProcessId);
    TerminateProcess(helper.value, 124);
    WaitForSingleObject(helper.value, 1000);
    DeleteFileW(path.c_str()); return;
  }
  DWORD status = 0;
  if (!GetExitCodeProcess(helper.value, &status)) { result.dump_error = GetLastError(); return; }
  if (status) { result.dump_error = status; DeleteFileW(path.c_str()); return; }
  result.timeout_dump = path;
  log::warn("launch", "Worker timeout dump: {}", paths::utf8(path));
}
}
std::optional<int> run_runtime_dump_helper(int argc, char** argv) {
  if (argc != 4 || std::string_view(argv[1]) != "--runtime-dump-helper") return std::nullopt;
  try { return write_timeout_dump(std::stoul(argv[2]), paths::from_utf8(argv[3])); }
  catch (...) { return ERROR_INVALID_PARAMETER; }
}
bool request_runtime_stall_dump(unsigned timeout_ms) {
  const auto name = "Local\\Venture.StallDump." + std::to_string(GetCurrentProcessId());
  Handle event(OpenEventA(EVENT_MODIFY_STATE, FALSE, name.c_str()));
  Handle complete(OpenEventA(SYNCHRONIZE, FALSE, (name + ".Complete").c_str()));
  if (!event.value || !complete.value) {
    log::info("startup-snapshot", "native dump request unavailable error={}", GetLastError());
    return false;
  }
  // Completion is manual-reset: repeated requests acknowledge the same one-shot
  // capture without requesting another dump or waiting for a nonexistent one.
  if (WaitForSingleObject(complete.value, 0) == WAIT_OBJECT_0) return true;
  if (!SetEvent(event.value)) return false;
  const auto waited = WaitForSingleObject(complete.value, timeout_ms);
  log::info("startup-snapshot", "native dump attempt completed={} wait={}", waited == WAIT_OBJECT_0, waited);
  return waited == WAIT_OBJECT_0;
}
void set_runtime_restart_channel(std::uintptr_t channel) { restart_channel = channel; }
void set_runtime_launch_channel(std::uintptr_t channel) { launch_channel = channel; }
void set_runtime_teardown_channel(std::uintptr_t channel) { teardown_channel = channel; }
namespace {
void notify_worker(std::string_view packet) noexcept {
  if (!teardown_channel) return;
  DWORD count = 0;
  // The diagnostic channel is a nonblocking message pipe. A full/broken pipe
  // drops the whole small record; neither a partial frame nor an exception can
  // obstruct the process exit. Restart uses its separate reliable channel.
  WriteFile(reinterpret_cast<HANDLE>(teardown_channel), packet.data(), DWORD(packet.size()), &count, nullptr);
}
}
void notify_runtime_teardown_posted() { notify_worker(std::string_view("\1", 1)); }
void notify_runtime_save_wait() { notify_worker(std::string_view("\3", 1)); }
const char* runtime_exit_wait_name(RuntimeExitWait wait) {
  switch (wait) {
    case RuntimeExitWait::arbitration: return "exit-arbitration";
    case RuntimeExitWait::cache_flush: return "cache-flush";
    case RuntimeExitWait::profiler_flush: return "profiler-flush";
    case RuntimeExitWait::restart_publication: return "restart-publication";
    case RuntimeExitWait::log_flush: return "log-flush";
    case RuntimeExitWait::kernel_exit: return "kernel-exit";
    case RuntimeExitWait::audio_capture_flush: return "audio-capture-flush";
    case RuntimeExitWait::av_flush: return "av-flush";
    case RuntimeExitWait::gpu_telemetry_flush: return "gpu-telemetry-flush";
    case RuntimeExitWait::guest_execution_stop: return "guest-execution-stop";
    case RuntimeExitWait::java_execution_stop: return "java-execution-stop";
    case RuntimeExitWait::save_flush: return "save-flush";
    case RuntimeExitWait::audio_shutdown: return "audio-shutdown";
    case RuntimeExitWait::vulkan_shutdown: return "vulkan-shutdown";
    case RuntimeExitWait::xr_shutdown: return "xr-shutdown";
    case RuntimeExitWait::os_termination: return "os-termination-after-destroy";
    default: return "lifecycle-teardown";
  }
}
void notify_runtime_exit_wait(RuntimeExitWait wait, int status) {
  char packet[10]{2, static_cast<char>(wait)};
  const auto tid = profile_thread_id();
  for (unsigned i = 0; i != 4; ++i) {
    packet[2+i] = char(tid >> (8*i)); packet[6+i] = char(unsigned(status) >> (8*i));
  }
  notify_worker(std::string_view(packet, sizeof(packet)));
}
[[noreturn]] void terminate_runtime_process(int status) {
  notify_runtime_exit_wait(RuntimeExitWait::kernel_exit, status);
  // ExitProcess runs DLL detach callbacks and can deadlock on locks held by
  // terminated host threads. Android exit_group has no host-DLL detach contract.
  TerminateProcess(GetCurrentProcess(), static_cast<UINT>(status));
  std::_Exit(status); // Failure fallback remains bounded by the supervisor.
 }
std::string take_runtime_launch_payload() {
  if (!launch_channel) return {};
  Handle channel(reinterpret_cast<HANDLE>(std::exchange(launch_channel, 0)));
  std::string payload(runtime_launch_payload_limit, '\0');
  DWORD count = 0;
  if (!ReadFile(channel.value, payload.data(), static_cast<DWORD>(payload.size()), &count, nullptr))
    failure("read launch intent");
  payload.resize(count);
  return payload;
}
[[noreturn]] void restart_runtime_process() { restart_runtime_process({}); }
[[noreturn]] void restart_runtime_process(std::string_view launch_payload) {
  notify_runtime_exit_wait(RuntimeExitWait::arbitration, 0);
  std::lock_guard decision(worker_exit_mutex);
  if (!restart_channel) throw std::runtime_error("Application restart requires a runtime supervisor");
  if (launch_payload.size() > runtime_launch_payload_limit) throw std::length_error("Launch intent too large");
  notify_runtime_exit_wait(RuntimeExitWait::restart_publication, 0);
  const std::string packet = std::string(1, '\1') + std::string(launch_payload);
  DWORD count = 0;
  if (!WriteFile(reinterpret_cast<HANDLE>(restart_channel), packet.data(), static_cast<DWORD>(packet.size()), &count, nullptr))
    failure("publish application restart");
  if (count != packet.size())
    throw std::system_error(ERROR_WRITE_FAULT, std::system_category(), "publish application restart");
  notify_runtime_exit_wait(RuntimeExitWait::log_flush, 0);
  flush_guest_process_files(0);
  const auto status = guest_process_exit_status();
  terminate_runtime_process(status > 0 ? status : 0);
}
RuntimeChildResult run_runtime_child(const std::filesystem::path& executable,
    const std::vector<std::string>& arguments, const std::vector<std::uintptr_t>& inherited,
    const std::function<bool()>& stop) {
  return run_runtime_child(executable, arguments, inherited, stop, {});
}
RuntimeChildResult run_runtime_child(const std::filesystem::path& executable,
    const std::vector<std::string>& arguments, const std::vector<std::uintptr_t>& inherited,
    const std::function<bool()>& stop, std::string_view launch_payload, bool acknowledged_teardown,
    const std::filesystem::path& diagnostic_run_directory) {
  if (launch_payload.size() > runtime_launch_payload_limit) throw std::length_error("Launch intent too large");
  SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
  HANDLE read_raw = nullptr, write_raw = nullptr;
  if (!CreatePipe(&read_raw, &write_raw, &security, runtime_launch_payload_limit + 1)) failure("create restart pipe");
  Handle read(read_raw), write(write_raw);
  if (!SetHandleInformation(read.value, HANDLE_FLAG_INHERIT, 0)) failure("protect restart pipe");
  Handle teardown_read, teardown_write;
  if (acknowledged_teardown) {
    static std::atomic<unsigned> sequence{0};
    const auto name = L"\\\\.\\pipe\\Venture.ExitTelemetry." + std::to_wstring(GetCurrentProcessId()) +
        L"." + std::to_wstring(sequence.fetch_add(1));
    teardown_read.value = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 4096, 4096, 0, nullptr);
    if (teardown_read.value == INVALID_HANDLE_VALUE) failure("create lifecycle telemetry");
    teardown_write.value = CreateFileW(name.c_str(), GENERIC_WRITE, 0, &security, OPEN_EXISTING, 0, nullptr);
    if (teardown_write.value == INVALID_HANDLE_VALUE) failure("open lifecycle telemetry");
    DWORD mode = PIPE_NOWAIT;
    if (!SetNamedPipeHandleState(teardown_write.value, &mode, nullptr, nullptr)) failure("make lifecycle telemetry nonblocking");
  }
  Handle launch_read;
  if (!launch_payload.empty()) {
    HANDLE writer_raw = nullptr;
    if (!CreatePipe(&launch_read.value, &writer_raw, &security, runtime_launch_payload_limit + 1))
      failure("create launch intent pipe");
    Handle writer(writer_raw);
    if (!SetHandleInformation(writer.value, HANDLE_FLAG_INHERIT, 0)) failure("protect launch intent pipe");
    DWORD count = 0;
    if (!WriteFile(writer.value, launch_payload.data(), static_cast<DWORD>(launch_payload.size()), &count, nullptr))
      failure("write launch intent");
    if (count != launch_payload.size()) throw std::runtime_error("Incomplete launch intent write");
  }
  Handle job(CreateJobObjectW(nullptr, nullptr));
  if (!job.value) failure("create runtime job");
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
    failure("configure runtime job");
  auto exe = std::filesystem::absolute(executable);
  auto command = quote_windows_arg(exe.wstring());
  for (const auto& arg : arguments) command += L" " + quote_windows_arg(paths::from_utf8(arg).wstring());
  command += L" --runtime-restart-write " + std::to_wstring(reinterpret_cast<std::uintptr_t>(write.value));
  if (teardown_write.value)
    command += L" --runtime-teardown-write " + std::to_wstring(reinterpret_cast<std::uintptr_t>(teardown_write.value));
  if (launch_read.value)
    command += L" --runtime-launch-read " + std::to_wstring(reinterpret_cast<std::uintptr_t>(launch_read.value));
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  HANDLE streams[3]{};
  struct CloseStreams { HANDLE* values; ~CloseStreams() { for (int i = 0; i < 3; ++i) if (values[i]) CloseHandle(values[i]); } } close{streams};
  const DWORD ids[]{STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
  std::vector<HANDLE> handles{write.value};
  if (teardown_write.value) handles.push_back(teardown_write.value);
  if (launch_read.value) handles.push_back(launch_read.value);
  for (int i = 0; i < 3; ++i) {
    const auto source = GetStdHandle(ids[i]);
    if (source && source != INVALID_HANDLE_VALUE) {
      if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &streams[i], 0, TRUE,
                           DUPLICATE_SAME_ACCESS)) failure("inherit runtime stream");
      handles.push_back(streams[i]);
    }
  }
  startup.StartupInfo.hStdInput = streams[0];
  startup.StartupInfo.hStdOutput = streams[1];
  startup.StartupInfo.hStdError = streams[2];
  for (const auto value : inherited) {
    auto handle = reinterpret_cast<HANDLE>(value);
    if (!SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) failure("inherit credential channel");
    handles.push_back(handle);
  }
  SIZE_T bytes = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
  std::vector<char> attributes(bytes);
  startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
  if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes)) failure("initialize runtime inheritance");
  struct DeleteAttributes { LPPROC_THREAD_ATTRIBUTE_LIST value; ~DeleteAttributes() { DeleteProcThreadAttributeList(value); } } cleanup{startup.lpAttributeList};
  if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
      handles.data(), handles.size() * sizeof(HANDLE), nullptr, nullptr)) failure("configure runtime inheritance");
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
      CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &process))
    failure("start app worker");
  Handle child(process.hProcess), thread(process.hThread);
  if (!AssignProcessToJobObject(job.value, child.value)) {
    auto error = GetLastError(); TerminateProcess(child.value, 1); SetLastError(error); failure("own app worker");
  }
  const auto* configured_run = environment::getenv("VENTURE_RUNTIME_RUN_DIR");
  const auto diagnostics = runtime_dump_directory(!diagnostic_run_directory.empty()
      ? diagnostic_run_directory
      : configured_run ? paths::from_utf8(configured_run) : std::filesystem::path{});
  const auto dump_event_name = "Local\\Venture.StallDump." + std::to_string(process.dwProcessId);
  Handle dump_event(diagnostics.empty() ? nullptr : CreateEventA(nullptr, FALSE, FALSE, dump_event_name.c_str()));
  Handle dump_complete(diagnostics.empty() ? nullptr : CreateEventA(nullptr, TRUE, FALSE, (dump_event_name + ".Complete").c_str()));
  if (!diagnostics.empty() && (!dump_event.value || !dump_complete.value)) failure("create startup capture events");
  bool dumped = false;
  if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) failure("resume app worker");
  CloseHandle(std::exchange(write.value, nullptr));
  if (teardown_write.value) CloseHandle(std::exchange(teardown_write.value, nullptr));
  RuntimeTeardownGrace grace(acknowledged_teardown);
  bool stop_requested = false, stop_signalled = false;
  for (;;) {
    const auto waited = WaitForSingleObject(child.value, 50);
    if (waited == WAIT_OBJECT_0) break;
    if (waited == WAIT_FAILED) failure("wait for app worker");
    if (!dumped && dump_event.value && WaitForSingleObject(dump_event.value, 0) == WAIT_OBJECT_0) {
      dumped = true;
      RuntimeChildResult snapshot{0, false, {}};
      timeout_dump(executable, process.dwProcessId, job.value, diagnostics, snapshot);
      log::info("startup-snapshot", "native capture attempt worker={} error={} complete=true", process.dwProcessId, snapshot.dump_error);
      SetEvent(dump_complete.value);
    }
    const auto now = RuntimeTeardownGrace::Clock::now();
    if (!stop_requested && stop()) {
      stop_requested = true;
      grace.request(now);
    }
    // Retain one-shot cancellation and retry until the worker's event exists.
    if (stop_requested && !stop_signalled)
      stop_signalled = request_runtime_stop(process.dwProcessId);
    if (teardown_read.value) {
      DWORD available = 0;
      if (!PeekNamedPipe(teardown_read.value, nullptr, 0, nullptr, &available, nullptr)) {
        if (GetLastError() != ERROR_BROKEN_PIPE) failure("inspect lifecycle acknowledgement");
      } else if (available) {
        char bytes[256]; DWORD count = 0;
        if (!ReadFile(teardown_read.value, bytes, std::min<DWORD>(available, sizeof(bytes)), &count, nullptr))
          failure("read worker exit stage");
        grace.consume(std::span(bytes, count), now, [&](auto wait, auto tid, auto status) {
          log::info("launch", "Worker {} post-exit waiter={} stage={} status={}", process.dwProcessId, tid, runtime_exit_wait_name(wait), status);
        });
      }
    }
    if (grace.expired(now)) {
      RuntimeChildResult result{124, false};
      result.supervisor_stopped = true;
      result.exit_wait = grace.exit_wait; result.exit_wait_tid = grace.exit_wait_tid;
      timeout_dump(executable, process.dwProcessId, job.value, runtime_dump_directory(diagnostic_run_directory), result);
      if (!TerminateJobObject(job.value, 124)) failure("stop app worker");
      if (WaitForSingleObject(child.value, 5000) != WAIT_OBJECT_0)
        log::warn("launch", "Worker {} post-exit waiter=supervisor stage=kernel-termination-completion still pending", process.dwProcessId);
      return result;
    }
  }
  DWORD status = 0, available = 0, count = 0;
  // Consume the final outcome even if the worker exited between supervisor
  // polls. Waiting for pipe EOF is unnecessary and could wait on descendants.
  if (teardown_read.value) {
    DWORD pending = 0;
    while (PeekNamedPipe(teardown_read.value, nullptr, 0, nullptr, &pending, nullptr) && pending) {
      char bytes[256]; DWORD received = 0;
      if (!ReadFile(teardown_read.value, bytes, std::min<DWORD>(pending, sizeof(bytes)), &received, nullptr) || !received) break;
      grace.consume(std::span(bytes, received), std::chrono::steady_clock::now(), [](auto, auto, auto) {});
    }
  }
  auto finish_result = [&](RuntimeChildResult result) {
    result.os_termination = grace.os_termination;
    return result;
  };
  if (!GetExitCodeProcess(child.value, &status)) failure("read app worker status");
  // Never wait for EOF: a child may have passed the writer to another process.
  if (!PeekNamedPipe(read.value, nullptr, 0, nullptr, &available, nullptr)) {
    if (GetLastError() != ERROR_BROKEN_PIPE) failure("inspect application restart");
    return finish_result({static_cast<int>(status), false});
  }
  if (available) {
    if (available > runtime_launch_payload_limit + 1) throw std::runtime_error("Invalid application restart size");
    std::string packet(available, '\0');
    if (!ReadFile(read.value, packet.data(), available, &count, nullptr)) failure("read application restart");
    if (count != available || packet[0] != 1) throw std::runtime_error("Invalid application restart notification");
    return finish_result({static_cast<int>(status), true, packet.substr(1)});
  }
  return finish_result({static_cast<int>(status), false});
}
}
