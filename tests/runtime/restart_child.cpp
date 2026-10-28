#include "core/process_exit.h"
#include "core/address_space.h"
#include "core/signals.h"
#include "core/av_telemetry.h"
#include "hle/libc/vfs.h"
#include "hle/registry.h"
#include "common/environment.h"
#include "common/paths.h"
#include "common/log.h"
#include "audio/audio.h"
#include "hle/vulkan/timing.h"
#include <array>
#include "core/thread_runtime.h"
#include "core/startup_stack.h"
#include "common/first_frame_progress.h"
#include <atomic>
#include <cstdlib>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
#endif
#include <mutex>
#include "platform/runtime_child.h"
#include "platform/runtime_stop.h"
#include "common/ipc.h"
#include "common/environment.h"
#include "common/paths.h"
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
int main(int argc, char** argv) {
  if (argc == 4 && std::string_view(argv[1]) == "--runtime-dump-helper" &&
      std::getenv("VENTURE_TEST_DUMP_HELPER_HANG"))
    std::this_thread::sleep_for(std::chrono::seconds(30));
  if (auto status = venture::platform::run_runtime_dump_helper(argc, argv))
    venture::platform::terminate_runtime_process(*status);
  for (int i = 1; i + 1 < argc; ++i)
    if (std::string(argv[i]) == "--runtime-restart-write")
      venture::platform::set_runtime_restart_channel(std::stoull(argv[i + 1]));
    else if (std::string(argv[i]) == "--runtime-launch-read")
      venture::platform::set_runtime_launch_channel(std::stoull(argv[i + 1]));
    else if (std::string(argv[i]) == "--runtime-teardown-write")
      venture::platform::set_runtime_teardown_channel(std::stoull(argv[i + 1]));
  if (argc < 2) return 1;
  const std::string mode = argv[1];
  if (mode == "os-outcome") {
    venture::platform::notify_runtime_exit_wait(venture::platform::RuntimeExitWait::os_termination, 0);
    venture::platform::terminate_runtime_process(0);
  }
  if (mode == "abort-reporter-exit-zero") {
    using namespace venture;
    set_terminal_guest_process_exit(true);
    GuestThread guest(1000);
    guest.attach_to_current_host_thread();
    register_all_hle_libraries();
    auto* libc = Linker::get().open("libc.so", kRtldNow, nullptr);
    const auto handler = AddressSpace::get().allocate(4096, kProtRead | kProtWrite | kProtExec, "[abort-reporter]");
    if (!libc || !handler) return 2;
    auto* code = g2h<std::uint32_t>(handler);
    code[0] = 0x52800000; code[1] = 0x58000070; code[2] = 0xd61f0200; code[3] = 0xd503201f;
    *g2h<std::uint64_t>(handler + 16) = libc->find_symbol("exit");
    signals::Action action{}; action.handler = handler;
    if (signals::set_action(6, &action, nullptr)) return 3;
    guest.call(libc->find_symbol("abort"), {});
    return 4;
  }
  if (mode == "host-quit-backstop" || mode == "host-quit-backstop-signal") {
    venture::set_terminal_guest_process_exit(true);
    venture::GuestThread::adopt_current();
    // Any per-thread arena/TLS cleanup on this process-ending path is a bug.
    venture::set_guest_thread_native_cleanup([] { std::abort(); });
    if (mode == "host-quit-backstop-signal") venture::record_guest_process_failure(139);
    venture::end_guest_process_after_grace();
  }
  if (mode == "--game") {
    // Synthetic no-frame runtime through the real harness's process launcher.
    // Match main.cpp: pass only DIAGNOSTIC_RUN_DIR as an explicit argument.
    const auto* configured = venture::environment::getenv("VENTURE_DIAGNOSTIC_RUN_DIR");
    if (!configured || venture::environment::getenv("VENTURE_RUNTIME_RUN_DIR")) return 2;
    return venture::platform::run_runtime_child(venture::paths::executable(),
        {"harness-no-frame"}, {}, [] { return false; }, {}, true,
        venture::paths::from_utf8(configured)).status;
  }
  if (mode == "harness-no-frame") {
    using namespace venture;
    const auto* configured = environment::getenv("VENTURE_DIAGNOSTIC_RUN_DIR");
    if (!configured || startup_diagnostics::first_frame_timeout_seconds() != 1) return 3;
    const auto root = paths::from_utf8(configured);
    log::set_file(paths::utf8(root / "runtime.log"));
    // This stack record is inspected in the native dump, not inferred from
    // timestamp/file ordering. A late dump would contain stage 2 instead.
    volatile std::uint64_t capture_stage[]{0x4e4f4652414d4531ull, 1, 0x5354414c4c535441ull};
    // Expose the actual array address so the optimizer must materialize the
    // contiguous record and the test can read its virtual stack location.
    paths::atomic_write(root / "fixture-stage-address.txt",
        std::to_string(reinterpret_cast<std::uintptr_t>(&capture_stage)));
    GuestThread guest(62921); guest.attach_to_current_host_thread();
    guest.cpu().set_pc(0x111110); guest.cpu().set_lr(0x444440); guest.cpu().set_sp(0);
    std::atomic<bool> held{false}, release{false};
    std::thread holder([&] {
      for_each_guest_thread([&](GuestThread&) {
        held.store(true);
        while (!release.load()) std::this_thread::yield();
      });
    });
    while (!held.load()) std::this_thread::yield();
    const auto owner = thread_registry_owner();
    const bool owner_valid = owner.stable && owner.tid != 0;
    startup_diagnostics::FirstFrameTrigger trigger;
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const bool fired = trigger.poll(1, startup_diagnostics::first_frame_timeout_seconds(), false, true);
    const bool complete = fired && signals::capture_first_frame_stall();
    release.store(true); holder.join();
    signals::try_log_guest_threads_raw("fixture-after-capture");
    capture_stage[1] = 2;
    log::info("launch", "Shutdown requested; posting Android lifecycle teardown");
    log::info("startup-snapshot", "fixture capture={} owner_tracked={} stage={}", complete, owner_valid,
              static_cast<std::uint64_t>(capture_stage[1]));
    log::flush();
    return complete && owner_valid ? 0 : 4;
  }
#ifdef _WIN32
  if (mode == "stall-ack-timeout") {
    const auto name = "Local\\Venture.StallDump." + std::to_string(GetCurrentProcessId());
    HANDLE request = CreateEventA(nullptr, FALSE, FALSE, name.c_str());
    HANDLE complete = CreateEventA(nullptr, TRUE, FALSE, (name + ".Complete").c_str());
    const auto began = std::chrono::steady_clock::now();
    const bool acknowledged = venture::platform::request_runtime_stall_dump(50);
    const auto elapsed = std::chrono::steady_clock::now() - began;
    CloseHandle(request); CloseHandle(complete);
    return !acknowledged && elapsed < std::chrono::seconds(2) ? 0 : 1;
  }
#endif
  if (mode == "stall-dump" || mode == "harness-dump") {
    const auto* configured = mode == "harness-dump" && argc > 2 ? argv[2]
        : venture::environment::getenv("VENTURE_RUNTIME_RUN_DIR");
    if (!configured) return 1;
    const auto dumps = venture::platform::runtime_dump_directory(venture::paths::from_utf8(configured));
    if (dumps.empty()) return 1;
    venture::platform::request_runtime_stall_dump();
    venture::platform::request_runtime_stall_dump();
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < until) {
      std::error_code error;
      if (std::filesystem::is_directory(dumps, error))
        for (const auto& entry : std::filesystem::directory_iterator(dumps))
          if (entry.path().extension() == ".dmp" && entry.file_size() > 32) return 0;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return 1;
  }
  if (mode == "telemetry-overload") {
#ifdef _WIN32
    const auto name = L"\\\\.\\pipe\\Venture.TelemetryFixture." + std::to_wstring(GetCurrentProcessId());
    HANDLE reader = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_INBOUND,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_BYTE | PIPE_NOWAIT, 1, 4096, 4096, 0, nullptr);
    if (reader == INVALID_HANDLE_VALUE) return 1;
    HANDLE writer = CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    DWORD mode = PIPE_NOWAIT;
    if (writer == INVALID_HANDLE_VALUE || !SetNamedPipeHandleState(writer, &mode, nullptr, nullptr)) return 2;
    venture::platform::set_runtime_teardown_channel(reinterpret_cast<std::uintptr_t>(writer));
#else
    int pipe_fds[2]; if (pipe(pipe_fds)) return 1;
    if (fcntl(pipe_fds[1], F_SETFL, O_NONBLOCK) < 0) return 2;
    venture::platform::set_runtime_teardown_channel(pipe_fds[1]);
#endif
    venture::platform::notify_runtime_exit_wait(venture::platform::RuntimeExitWait::arbitration, 0);
    char initial[10]{};
#ifdef _WIN32
    DWORD count = 0;
    if (!ReadFile(reader, initial, sizeof(initial), &count, nullptr) || count != sizeof(initial)) return 3;
#else
    if (read(pipe_fds[0], initial, sizeof(initial)) != sizeof(initial)) return 3;
#endif
    if (initial[0] != 2) return 4;
    // No reader consumes further records: exhaust capacity, then break it.
    for (int i = 0; i != 10000; ++i)
      venture::platform::notify_runtime_exit_wait(venture::platform::RuntimeExitWait::arbitration, 0);
#ifdef _WIN32
    CloseHandle(reader);
#else
    close(pipe_fds[0]);
#endif
    venture::platform::notify_runtime_teardown_posted();
    venture::platform::notify_runtime_exit_wait(venture::platform::RuntimeExitWait::log_flush, 0);
    venture::platform::restart_runtime_process("telemetry-survived");
  }
  if (mode == "guest-file-exit") {
    if (argc < 3) return 2;
    using namespace venture;
    const auto root = paths::from_utf8(argv[2]);
    std::filesystem::create_directories(root / "files");
    const auto events = paths::utf8(root / "events.jsonl");
    const auto wav = paths::utf8(root / "mix.wav");
    const auto gpu = paths::utf8(root / "gpu.jsonl");
#ifdef _WIN32
    _putenv_s("VENTURE_AV_TELEMETRY", events.c_str());
    _putenv_s("VENTURE_AUDIO_CAPTURE_WAV", wav.c_str());
    _putenv_s("VENTURE_VULKAN_GPU_TIMING", gpu.c_str());
#else
    setenv("VENTURE_AV_TELEMETRY", events.c_str(), 1);
    setenv("VENTURE_AUDIO_CAPTURE_WAV", wav.c_str(), 1);
    setenv("VENTURE_VULKAN_GPU_TIMING", gpu.c_str(), 1);
#endif
    environment::refresh();
    log::set_file(paths::utf8(root / "runtime.log"));
    process_config().package = "test.exit";
    process_config().data_dir = root;
    GuestThread guest(1000);
    guest.attach_to_current_host_thread();
    register_all_hle_libraries();
    auto* libc = Linker::get().open("libc.so", kRtldNow, nullptr);
    auto invoke = [&](const char* name, std::initializer_list<std::uint64_t> args) {
      return guest.call(libc->find_symbol(name), GuestCallArgs{std::span(args.begin(), args.size()), {}}).x0;
    };
    const auto path = process_config().guest_data_dir() + "/files/save.bin";
    const auto file = invoke("fopen", {h2g(path.c_str()), h2g("wb")});
    if (!file) return 3;
    std::array<char, 8192> buffer{};
    if (invoke("setvbuf", {file, h2g(buffer.data()), 0, buffer.size()})) return 4;
    const char bytes[] = "guest-save-without-fclose";
    if (invoke("fwrite", {h2g(bytes), 1, sizeof(bytes)-1, file}) != sizeof(bytes)-1) return 5;
    if (!audio::Backend::get().use_null_device()) return 6;
    audio::Config config; config.format = audio::Format::f32; config.capacity = 4800;
    int result = 0;
    auto stream = audio::Stream::create(config, &result);
    if (!stream || result) return 7;
    std::array<float, 9600> pcm; pcm.fill(0.25f);
    // Capacity is the storage ceiling; the default writable watermark is only
    // two negotiated bursts. Explicitly enlarge it for this finite WAV fixture.
    if (stream->set_buffer_size(config.capacity) != config.capacity) return 8;
    if (stream->write(pcm.data(), config.capacity) != std::uint32_t(config.capacity) || stream->start()) return 8;
    const auto bound = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (audio::Backend::get().mixer_stats().non_silent_samples < 1920 && std::chrono::steady_clock::now() < bound)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (audio::Backend::get().mixer_stats().non_silent_samples < 1920) return 9;
    for (int i = 0; i != 1024; ++i) av::emit({{"kind", "exit-proof"}, {"sequence", i}});
    log::info("exit-proof", "buffered-final-line");
    vulkan::timing_host_wait("exit-proof", 7);
    set_guest_exit_capture_flush([] { audio::Backend::get().finish_capture(); vulkan::finish_timing(); });
    set_terminal_guest_process_exit(true);
    // Exercise the guest's actual libc exit thunk; deliberately no fclose.
    invoke("exit", {0});
    return 10;
  }
  if (mode == "restart-race" || mode == "restart-race-control") {
    // The JNI exit entry reserves this decision before claiming the intent.
    // Hold the claimed payload while a second caller takes the native exit path.
    std::unique_lock reservation(venture::worker_exit_mutex, std::defer_lock);
    if (mode == "restart-race") reservation.lock();
    const std::string claimed_payload = "claimed-cold-launch";
    venture::set_terminal_guest_process_exit(true);
    std::atomic<bool> attempted = false;
    std::thread concurrent([&] {
      attempted.store(true, std::memory_order_release);
      venture::exit_guest_process(0);
    });
    while (!attempted.load(std::memory_order_acquire)) std::this_thread::yield();
    // A lost arbitration kills this child here, before packet publication.
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    venture::platform::restart_runtime_process(claimed_payload);
  }
  if (mode == "received-cold-launch") {
    return venture::platform::take_runtime_launch_payload() == "claimed-cold-launch" ? 0 : 1;
  }
  if (mode == "post-exit-wait") {
    venture::platform::notify_runtime_exit_wait(venture::platform::RuntimeExitWait::log_flush, 0);
    std::this_thread::sleep_for(std::chrono::seconds(30)); return 1;
  }
  if (mode == "detach-hang" || mode == "detach-control") {
#ifdef _WIN32
    if (argc < 3 || !LoadLibraryA(argv[2])) return 1;
#endif
    if (mode == "detach-control") std::_Exit(0);
    venture::platform::terminate_runtime_process(0);
  }
  if (mode == "slow-diagnostics") {
    venture::platform::RuntimeStop stop(0);
    while (!stop.requested()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::this_thread::sleep_for(std::chrono::seconds(11));
    venture::platform::notify_runtime_teardown_posted();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return 37;
  }
  if (mode == "graceful" || mode == "graceful-delayed") {
    if (mode == "graceful-delayed") std::this_thread::sleep_for(std::chrono::milliseconds(400));
    venture::platform::RuntimeStop stop(0);
    while (!stop.requested()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return 0;
  }
  if (mode == "intent") {
    auto payload = venture::platform::take_runtime_launch_payload();
    if (!venture::platform::take_runtime_launch_payload().empty()) return 1;
    venture::platform::restart_runtime_process(payload);
  }
  if (mode == "broker" || mode == "broker-restart") {
    if (argc < 4) return 1;
    auto channel = venture::PipeChannel::inherited(std::stoull(argv[2]), std::stoull(argv[3]));
    if (channel->call(venture::Json{{"op", "fixture"}}) != venture::Json{{"reply", "retained"}}) return 1;
    if (mode == "broker-restart") venture::platform::restart_runtime_process();
    return 0;
  }
  if (mode == "failed-restart") {
    venture::record_guest_process_failure(139);
    venture::platform::restart_runtime_process("failed-cold-launch");
  }
  if (mode == "restart") venture::platform::restart_runtime_process();
  if (mode == "wait") { std::this_thread::sleep_for(std::chrono::seconds(30)); return 1; }
  if (mode == "arguments") return argc >= 3 && std::string(argv[2]) == "a space, a quote \" and \\ path" ? 0 : 1;
  return 85; // A genuine app status must never look like a restart.
}
