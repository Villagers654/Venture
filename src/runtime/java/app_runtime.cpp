#include "common/environment.h"
#include "common/first_frame_progress.h"
#include "core/startup_stack.h"
#include "java/app_runtime.h"
#include <jni.h>
#include <atomic>
#include <chrono>
#include <format>
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include <algorithm>
#include <optional>
#include <memory>

#include "common/log.h"
#include "common/paths.h"
#include "common/quit_bound.h"
#include "platform/runtime_child.h"
#include "java/support_files.h"
#include "core/process.h"
#include "core/hle_runtime.h"
#include "core/signals.h"
#include "core/thread_runtime.h"
#include "gfx/gpu_device.h"
#include "gfx/blob_cache.h"
#include "java/app_launcher.h"
#include "java/conversion.h"
#include "java/jvm.h"
#include "java/natives/framework_natives.h"
#include "java/natives/graphics_bridge.h"
#include "java/natives/sqlite_bridge.h"
#include "java/natives/runtime_bridge.h"
#include "platform/runtime_stop.h"
#include "core/profile.h"
#include "core/lifecycle_grace.h"
#include "xr/host_xr.h"
#include "xr/native.h"
#include "xr/session.h"
#include "android/choreographer.h"
#include "audio/audio.h"
#include "java/natives/display_bridge.h"
#include "hle/libc/libc_parts.h"
#include "hle/libc/vfs.h"
#include "core/os_quit.h"
#include "hle/vulkan/terminal_allocator.h"
#include "core/process_exit.h"
#include "java/execution_freeze.h"
#include "java/natives/audio_bridge.h"
#include "audio/audio.h"
#include "gfx/angle.h"
#include "gfx/gpu_device_internal.h"
#include "hle/vulkan/vulkan.h"
#include "platform/file_activity.h"
#include "xr/haptic_sequence.h"

namespace venture::java {
namespace {
// Set by the first successful xrWaitFrame: the game reached its XR frame loop.
std::atomic<bool> frame_loop_started{false};
std::string apk_label() {
  auto* env = host_env();
  auto cls = env->FindClass("com/venture/runtime/ApplicationIdentity");
  auto method = cls ? env->GetStaticMethodID(cls, "apkLabel", "(Ljava/lang/String;)[B") : nullptr;
  if (!method || env->ExceptionCheck()) {
    env->ExceptionClear();
    if (cls) env->DeleteLocalRef(cls);
    return {};
  }
  const auto path = process_config().apk.u16string();
  auto argument = env->NewString(reinterpret_cast<const jchar*>(path.data()), static_cast<jsize>(path.size()));
  auto bytes = method && argument ? static_cast<jbyteArray>(env->CallStaticObjectMethod(cls, method, argument)) : nullptr;
  std::string label;
  if (!env->ExceptionCheck() && bytes) {
    label.resize(env->GetArrayLength(bytes));
    env->GetByteArrayRegion(bytes, 0, static_cast<jsize>(label.size()), reinterpret_cast<jbyte*>(label.data()));
  }
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    log::warn("launch", "APK application label unavailable; using package identity");
    label.clear();
  }
  if (bytes) env->DeleteLocalRef(bytes);
  if (argument) env->DeleteLocalRef(argument);
  if (cls) env->DeleteLocalRef(cls);
  return label;
}
bool request_shutdown() {
  auto* env = host_env();
  if (!env) return false;
  auto cls = env->FindClass("com/venture/runtime/AppMain");
  // A menu quit removes the task (pause and stop, then OS termination); every
  // other shutdown finishes the Activity.
  const char* name = os_quit::enabled && os_quit::host_requested.load() ? "requestTaskRemoval" : "requestShutdown";
  auto method = cls ? env->GetStaticMethodID(cls, name, "()Z") : nullptr;
  bool ready = method && env->CallStaticBooleanMethod(cls, method);
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    ready = false;
  }
  if (cls) env->DeleteLocalRef(cls);
  return ready;
}
struct QuitProgress { int stages = 0, native_activities = 0; bool valid = false; };
QuitProgress quit_progress() {
  auto* env = host_env();
  if (!env) return {};
  auto cls = env->FindClass("com/venture/runtime/AppMain");
  auto progress = cls ? env->GetStaticMethodID(cls, "quitProgress", "()I") : nullptr;
  auto native_count = cls ? env->GetStaticMethodID(cls, "quitNativeCount", "()I") : nullptr;
  QuitProgress result;
  if (progress && native_count && !env->ExceptionCheck()) {
    result.stages = env->CallStaticIntMethod(cls, progress);
    if (!env->ExceptionCheck()) result.native_activities = env->CallStaticIntMethod(cls, native_count);
    result.valid = !env->ExceptionCheck();
  }
  if (env->ExceptionCheck()) { env->ExceptionClear(); result.valid = false; }
  if (cls) env->DeleteLocalRef(cls);
  return result;
}
bool os_host_calls_safe() {
  bool safe = true;
  for_each_guest_thread([&](GuestThread& thread) {
    const auto record = shutdown_trace::read(thread);
    if (!record.entered) return;
    const auto& call = hle::function(record.last_hle);
    const bool guest_wait = call.library == "libc.so" && record.wait != shutdown_trace::Wait::none;
    const bool looper_wait = call.library == "libandroid.so" &&
        (call.name == "ALooper_pollOnce" || call.name == "ALooper_pollAll");
    if (!guest_wait && !looper_wait) {
      log::error("launch", "OS quit rejected host call tid={} call={}!{}", thread.tid(), call.library, call.name);
      safe = false;
    }
  });
  return safe;
}
[[noreturn]] void os_quit_failed(std::string_view reason) {
  record_guest_process_failure(124);
  log::error("launch", "OS quit failed; quit=failed_os_termination reason={} (P1)", reason);
  shutdown_trace::snapshot("os-quit-failed");
  end_guest_process_after_grace();
}
// `saving` (optional): every busy guest thread is in a libc file write or sync.
bool native_save_work_quiet(std::string* blocker, bool* saving = nullptr) {
  bool quiet = true, only_files = true;
  auto block = [&](std::string what, bool file = false) {
    if (quiet && blocker) *blocker = std::move(what);
    quiet = false;
    only_files = only_files && file;
  };
  if (!try_for_each_guest_thread([&](GuestThread& thread) {
    if (!thread.os_call_depth.load()) return;
    const auto record = shutdown_trace::read(thread);
    // Translated guest code cannot reach a file without a host call, which is
    // recorded; a paused engine's own loop (UE4's game thread) may keep running.
    if (!record.entered) return;
    if (!record.last_hle) { block(std::format("tid {} in an unrecorded host call", thread.tid())); return; }
    const auto& call = hle::function(record.last_hle);
    const bool wait = record.entered && ((call.library == "libc.so" && record.wait != shutdown_trace::Wait::none) ||
        (call.library == "libandroid.so" && (call.name == "ALooper_pollOnce" || call.name == "ALooper_pollAll")));
    constexpr std::string_view writes[] = {"write", "pwrite", "pwrite64", "writev", "fsync", "fdatasync",
                                           "sync_file_range", "msync", "close", "fclose", "fflush", "rename"};
    if (!wait)
      block(std::format("tid {} running {}!{}", thread.tid(), call.library, call.name),
            call.library == "libc.so" && std::ranges::find(writes, call.name) != std::end(writes));
  })) block("guest thread census unavailable");
  if (saving) *saving = !quiet && only_files;
  return quiet;
}
void require_recorded_outcome(ExecutionFreeze& execution) {
  const auto java_status = execution.recorded_failure();
  if (!java_status) { record_guest_process_failure(1); os_quit_failed("Java failure status unavailable"); }
  if (auto fatal = recorded_guest_process_failure()) record_guest_process_failure(fatal);
  if (*java_status) record_guest_process_failure(*java_status);
  if (guest_process_exit_status() > 0) os_quit_failed("recorded guest or Java failure preceded OS termination");
}
[[noreturn]] void end_os_menu_quit(platform::FileActivity& files, ExecutionFreeze& freeze) {
  platform::notify_runtime_exit_wait(platform::RuntimeExitWait::arbitration, 0);
  std::lock_guard decision(worker_exit_mutex);
  try {
    require_recorded_outcome(freeze);
    if (platform::data_writers(process_config().data_dir) == platform::DataWriters::unknown)
      os_quit_failed("application data handle census unavailable");
    shutdown_trace::snapshot("os-termination-admission");
    platform::notify_runtime_exit_wait(platform::RuntimeExitWait::guest_execution_stop, 0);
    os_quit::stop_guest_execution();
    audio::Backend::get().guest_sources_ended();
    const auto drain_end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (os_quit::active_calls.load() && std::chrono::steady_clock::now() < drain_end)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (os_quit::active_calls.load() || os_quit::cleanup_failed.load())
      os_quit_failed("guest execution or owner-thread graphics release did not drain");
    // Read the framework acknowledgement before suspending application Java
    // threads. No Java code may be called after the suspension barrier.
    const auto progress = quit_progress();
    if (!progress.valid || (progress.stages & 15) != 15)
      os_quit_failed("lifecycle acknowledgement changed before Java stop");
    platform::notify_runtime_exit_wait(platform::RuntimeExitWait::java_execution_stop, 0);
    std::string error;
    if (!freeze.freeze(&error)) {
      log::error("launch", "OS quit JVM barrier: {}", error);
      os_quit_failed("JVM execution did not stop safely");
    }
    require_recorded_outcome(freeze);
    if (platform::data_writers(process_config().data_dir) == platform::DataWriters::unknown)
      os_quit_failed("application data handle census unavailable after execution stop");
    platform::notify_runtime_exit_wait(platform::RuntimeExitWait::save_flush, 0);
    if (!hle::libc::flush_stdio_for_os_quit() || !hle::libc::close_gzip_for_os_quit() ||
        !flush_sqlite_for_os_quit()) os_quit_failed("save flush or lifecycle acknowledgement failed");
    // Flushes can themselves publish data. Require a fresh, complete quiet
    // second after them; notifications supplement logical disk-write counters.
    auto quiet_since = std::chrono::steady_clock::now();
    const auto quiet_end = quiet_since + std::chrono::seconds(2);
    for (;;) {
      const auto change = files.poll();
      if (change == platform::FileActivity::Poll::failed) os_quit_failed("save watcher failed");
      const auto now = std::chrono::steady_clock::now();
      if (change == platform::FileActivity::Poll::changed || os_quit::active_writes.load()) quiet_since = now;
      if (now - quiet_since >= std::chrono::seconds(1)) break;
      if (now >= quiet_end) os_quit_failed("save writes did not become quiet");
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!os_host_calls_safe()) os_quit_failed("host call remained after execution stop");
    if (!vulkan::begin_terminal_allocator_bridge())
      os_quit_failed("Vulkan allocator ownership unavailable after execution stop");
    platform::notify_runtime_exit_wait(platform::RuntimeExitWait::vulkan_shutdown, 0);
    if (!vulkan::wait_devices_for_os_quit(std::chrono::seconds(2)))
      os_quit_failed("Vulkan queue/device idle failed or exceeded its bound");
    if (!vulkan::terminal_allocator_bridge_healthy())
      os_quit_failed("Vulkan allocator ownership failed during native wait");
    platform::notify_runtime_exit_wait(platform::RuntimeExitWait::xr_shutdown, 0);
    auto& session = xr::session();
    {
      std::lock_guard lock(session.mutex);
      if (session.handle) xr::cancel_sequence(session.handle);
      // DestroyInstance releases the runtime's sessions and swapchains. Guest
      // Vulkan devices/children stay intact for loader/driver process cleanup.
      if (!xr::shutdown_host_xr()) os_quit_failed("host OpenXR instance cleanup failed");
      session.handle = XR_NULL_HANDLE;
      session.owner = xr::SessionOwner::none;
      session.running = false;
    }
    if (!vulkan::terminal_allocator_bridge_healthy())
      os_quit_failed("Vulkan allocator ownership failed during XR shutdown");
    platform::notify_runtime_exit_wait(platform::RuntimeExitWait::audio_shutdown, 0);
    shutdown_audio_tracks();
    audio::Backend::get().shutdown();
    platform::notify_runtime_exit_wait(platform::RuntimeExitWait::vulkan_shutdown, 0);
    // Only the shared host-owned device is explicitly destroyed. Its native
    // children belong to Venture/ANGLE and are released by their owning layer.
    // A lost device cannot be torn down: ANGLE's eglTerminate recursed until a stack overflow after a GPU
    // reset (Cooking Simulator on SteamVR). Process termination releases it, as the driver expects.
    if (vulkan::os_quit_device_lost()) {
      log::warn("launch", "OS quit skips host graphics cleanup: the device was lost");
    } else {
      if (auto device = gfx::gpu_device_state().device) vulkan::release_os_device_resources(device);
      if (!gfx::shutdown_for_os_quit()) os_quit_failed("host graphics cleanup failed");
    }
    shutdown_trace::snapshot("os-termination-after-destroy");
    require_recorded_outcome(freeze);
    log::info("launch", "quit=os_task_removal; host resources closed");
    platform::notify_runtime_exit_wait(platform::RuntimeExitWait::os_termination, 0);
    flush_guest_process_files(0);
    if (!vulkan::terminal_allocator_bridge_healthy())
      os_quit_failed("Vulkan allocator ownership failed during host shutdown");
    platform::terminate_runtime_process(std::max(guest_process_exit_status(), 0));
  } catch (const std::exception& error) {
    log::error("launch", "OS quit host cleanup exception: {}", error.what());
    os_quit_failed("host cleanup exception");
  }
}
}
void launch_android_app(const AppLaunchOptions& options) {
  // Fail before Java conversion when the loader has no active runtime.
  // The bundled headless mock is used only when explicitly requested; a normal
  // launch uses the system's active OpenXR runtime and fails visibly without one.
  const std::string_view mode = venture::environment::getenv("VENTURE_XR_RUNTIME") ? venture::environment::getenv("VENTURE_XR_RUNTIME") : "";
  if (mode == "mock") {
    // Respect the loader's explicit override, including a test runtime outside
    // a shipping layout. The adjacent mock remains the development default.
    const auto* manifest = std::getenv("XR_RUNTIME_JSON");
    const auto mock = manifest && *manifest ? paths::from_utf8(manifest)
        : options.runtime_directory / "mock_openxr_runtime.json";
    if (!std::filesystem::is_regular_file(mock) || !xr::set_runtime_manifest_for_process(paths::utf8(mock)))
      throw std::runtime_error("Cannot select mock OpenXR runtime");
  } else if (!mode.empty() && mode != "host") {
    throw std::runtime_error("VENTURE_XR_RUNTIME must be mock or host");
  } else {
    // SteamVR and other runtimes report the headset unavailable while starting.
    xr::set_system_wait(std::chrono::seconds(30));
  }
  const auto support = support_files();
  std::string error;
  install_runtime_framework_bridges();
  register_framework_natives();
  register_sqlite_natives();
  register_graphics_natives();
  log::info("launch", "Starting HotSpot with Android framework and verified class loading");
  JvmOptions jvm{paths::utf8(options.java_home), {paths::utf8(support.framework), paths::utf8(support.bridge)},
                 {"-Dventure.displayPeriodNs=" +
                     std::to_string(xr::display_timing_state().period.load())}};
  if (process_config().embedded_java_scripts)
    jvm.agent_path = paths::utf8(support.agent);
  if (!start_jvm(jvm, &error)) throw std::runtime_error(error);
  xr::set_application_identity(apk_label(), process_config().package, process_config().version_code);
  log::info("launch", "Initializing host OpenXR");
  auto& xr = xr::host_xr();
  if (!xr.available) throw std::runtime_error(xr.error);
  auto class_path = options.class_path;
  if (class_path.empty() && !process_config().has_code) {
    // As on Android, the class loader's path is the APK itself. With no code the
    // manifest's activity is the framework's android.app.NativeActivity.
    log::info("launch", "APK has no Java code; its native activity starts through the framework");
    class_path.push_back(paths::utf8(process_config().apk));
  } else if (class_path.empty()) {
    log::info("launch", "Converting and verifying APK Java (cached)");
    ConversionOptions conversion;
    conversion.java = paths::utf8(support.dex_java);
    conversion.tools = paths::utf8(support.dex_tools);
    conversion.apk = paths::utf8(process_config().apk);
    conversion.package = process_config().package;
    conversion.cache = paths::utf8(process_config().cache_dir / "java");
    conversion.framework_jars = {paths::utf8(support.framework), paths::utf8(support.bridge)};
    conversion.hierarchy_metadata = {paths::utf8(support.hierarchy)};
    ConvertedApk converted;
    if (!convert_apk(conversion, &converted, &error)) {
      if (!converted.translation_complete) throw std::runtime_error(error);
      log::warn("launch", "APK has unresolved optional/framework dependencies; each reached class is verified by HotSpot. Details: {}", converted.report);
    }
    class_path.push_back(converted.app_jar);
  }
  std::string application_path;
  for (const auto& entry : class_path) {
    if (!application_path.empty()) application_path += VENTURE_JAVA_PATH_SEPARATOR;
    application_path += entry;
  }
  if (!options.defer_gpu) {
    log::info("launch", "Creating shared Vulkan device through {}", xr.runtime_name);
    auto& gpu = gfx::gpu_device();
    if (!gpu.device || !gpu.queue) throw std::runtime_error("Shared GPU initialization failed");
  } else log::info("launch", "Diagnostic: GPU creation deferred until guest graphics access");
  xr::set_display_observer([](std::int64_t period) {
    android::display_vsync(0, period);
    update_framework_display_period(period);
  });
  xr::set_frame_observer([](std::int64_t time, std::int64_t period) {
    frame_loop_started.store(true, std::memory_order_relaxed);
    android::display_vsync(time, period);
    dispatch_framework_xr_vsync(time, period);
  });
  struct ClearObserver { ~ClearObserver() { xr::set_frame_observer(nullptr); xr::set_display_observer(nullptr); } } clear_observer;

  platform::RuntimeStop stop(options.parent_pid);
  if (os_quit::enabled) os_quit::thread_cleanup = gfx::release_thread_for_os_quit;
  std::atomic<bool> closing{false}, host_stop{false};
  std::jthread monitor([&](std::stop_token token) {
    const auto started = std::chrono::steady_clock::now();
    const auto bound = configured_quit_backstop();
    LifecycleGrace grace(bound);
    std::unique_ptr<platform::FileActivity> files;
    std::unique_ptr<ExecutionFreeze> execution;
    bool menu_threads_quiet = false;
    auto menu_writers = platform::DataWriters::unknown;
    std::string menu_quiet_error;
    std::optional<std::chrono::steady_clock::time_point> destroy_delivered, save_began;
    std::optional<QuitQuietness> quietness;
    // A file save is the only remaining work (see save_quit_bound).
    bool saving = false, save_announced = false, recency_logged = false;
    QuitProgress menu_progress;
    bool menu_files_healthy = true;
    bool posted = false, interrupted = false, xr_stop_decided = false;
    startup_diagnostics::FirstFrameTrigger first_frame_probe;
    startup_diagnostics::FrameStallTrigger frame_stall_probe;
    const auto first_frame_bound = startup_diagnostics::first_frame_timeout_seconds();
    while (!token.stop_requested()) {
      const auto now = std::chrono::steady_clock::now();
      if (startup_diagnostics::enabled && !closing) startup_diagnostics::poll();
      const bool timed_out = options.run_seconds > 0 &&
          std::chrono::duration<double>(now - started).count() >= options.run_seconds;
      const bool stopping = stop.requested() || timed_out || guest_process_exit_status() >= 0;
      if (first_frame_probe.poll(std::chrono::duration_cast<std::chrono::seconds>(now - started).count(),
                                 first_frame_bound, startup_diagnostics::first_frame_submitted(), stopping)) {
        // The independent supervisor captures native stacks before any guest
        // registry/logging work or Android teardown can change the handshake.
        signals::capture_first_frame_stall();
      }
      if (frame_stall_probe.poll(std::chrono::duration_cast<std::chrono::seconds>(now - started).count(),
                                 startup_diagnostics::frames_submitted(), stopping || !startup_diagnostics::host_session_focused())) {
        signals::capture_frame_stall();
        vulkan::log_gpu_memory();
      }
      if (!closing && stopping) {
        closing = true;
        host_stop = stop.requested() || timed_out;
        if (os_quit::enabled && host_stop) {
          files = std::make_unique<platform::FileActivity>(process_config().data_dir);
          quietness.emplace(std::chrono::steady_clock::now());
        }
        shutdown_trace::phase("diagnostics");
        // Capture the running game before teardown interrupts its guest CPUs.
        profile::stop();
        gfx::flush_blob_cache();
        hle::write_census();
        if (venture::environment::getenv("VENTURE_TRACE_THREADS") && guest_process_exit_status() < 0)
          signals::log_guest_threads(timed_out ? "run time elapsed" : "stop requested");
        log::info("launch", "Shutdown requested; posting Android lifecycle teardown (host_quit={} backstop_ms={})",
                  host_stop.load(), bound.count());
        shutdown_trace::phase("request");
        shutdown_trace::begin();
      }
      if (os_quit::enabled && closing && host_stop && !xr_stop_decided) {
        const auto result = xr::session().request_host_stop();
        if (result != XR_ERROR_SESSION_NOT_READY) {
          xr_stop_decided = true;
          log::info("launch", "Host quit route=Android Activity lifecycle; XR stop result={}", int(result));
        }
      }
      if (closing && !posted && (!os_quit::enabled || !host_stop || xr_stop_decided)) {
        if (os_quit::enabled && host_stop) os_quit::host_requested = true;
        if (request_shutdown()) {
          posted = true;
          grace.posted(std::chrono::steady_clock::now());
          platform::notify_runtime_teardown_posted();
          shutdown_trace::phase("lifecycle-posted");
        }
      }
      auto clock = std::chrono::steady_clock::now();
      if (os_quit::enabled && posted && host_stop && files) {
        menu_progress = quit_progress();
        if (!execution) execution = std::make_unique<ExecutionFreeze>();
        // Threads and open writers are judged apart: an idle writer handle
        // delays termination (it may hold userspace buffers) but cannot hold
        // a removed task alive past the bound; a Quest kills it as it stands.
        bool java_saving = false, native_saving = false;
        const bool java_quiet = execution->application_quiet(&menu_quiet_error, &java_saving);
        menu_threads_quiet = java_quiet;
        menu_writers = platform::data_writers(process_config().data_dir);
        std::string native_blocker;
        bool native_quiet = true;
        if (java_quiet || java_saving) {
          native_quiet = native_save_work_quiet(&native_blocker, &native_saving);
          if (!native_quiet && java_quiet) {
            menu_threads_quiet = false;
            menu_quiet_error = "native execution runnable or wait state unknown: " + native_blocker;
          }
        }
        saving = (java_saving || !native_quiet) && (native_quiet || native_saving);
        if (saving && !save_began) {
          save_began = clock;
          log::info("launch", "Quit waits for a file save to finish (bound {} s): {}", save_quit_bound.count(),
                    java_saving ? menu_quiet_error : native_blocker);
          if (!save_announced) platform::notify_runtime_save_wait();
          save_announced = true;
        } else if (!saving && save_began) {
          log::info("launch", "Quit file save finished after {} ms",
                    std::chrono::duration_cast<std::chrono::milliseconds>(clock - *save_began).count());
          save_began.reset();
        }
        quietness->threads(menu_threads_quiet, clock);
        const auto change = files->poll();
        if (change == platform::FileActivity::Poll::failed) menu_files_healthy = false;
        if (change == platform::FileActivity::Poll::changed) quietness->data_changed(clock);
        const bool native_delivered = menu_progress.native_activities == 0 ||
            os_quit::native_destroy_count.load() >= static_cast<unsigned>(menu_progress.native_activities);
        if (menu_progress.valid && (menu_progress.stages & 7) == 7 && native_delivered && !destroy_delivered) {
          const auto ns = os_quit::native_destroy_ns.load();
          destroy_delivered = ns ? std::chrono::steady_clock::time_point(std::chrono::nanoseconds(ns)) : clock;
          log::info("launch", "OS quit lifecycle delivered; terminating after one quiet second (bound 8000 ms) native_activities={}", menu_progress.native_activities);
        }
      }
      bool expired = grace.expired(clock, saving);
      const auto sample = [&] {
        QuitSample s;
        s.destroy_delivered = destroy_delivered.has_value();
        s.stages_complete = menu_progress.valid && (menu_progress.stages & 15) == 15;
        s.files_healthy = menu_files_healthy;
        s.threads_quiet = execution && menu_threads_quiet;
        s.writers_known = menu_writers != platform::DataWriters::unknown;
        s.writers_closed = menu_writers == platform::DataWriters::closed;
        s.write_active = os_quit::active_writes.load() != 0;
        s.since_write = std::chrono::nanoseconds(os_quit::now_ns() - os_quit::last_write_ns.load());
        return s;
      };
      if (os_quit::enabled && host_stop && destroy_delivered) {
        // A Quest ends a removed task as soon as it has stopped. A paused game
        // still makes host calls (timers, looper wakes), so terminate after one
        // quiet second rather than on a single sample, within an 8 s bound.
        const auto current = sample();
        const bool settled = quietness && quit_settled(current, *quietness, clock);
        const bool recency = quietness && quit_recency_pending(current, *quietness, clock);
        const auto since = clock - *destroy_delivered;
        const bool waiting = (saving || recency) && grace.within_save_bound(clock);
        if (since >= destroy_settle_bound && recency && waiting && !recency_logged) {
          recency_logged = true;
          log::info("launch", "OS quit bound reached inside a quiet-second window ({}); waiting up to {} s for it",
                    quit_admission(current, *quietness, clock), save_quit_bound.count());
        }
        expired = settled || (since >= destroy_settle_bound && !waiting);
      }
      if (closing && !interrupted && expired) {
        if (os_quit::enabled && host_stop && files) {
          if (execution) require_recorded_outcome(*execution);
          const auto bound_sample = sample();
          if (const char* unsettled = quit_admission(bound_sample, *quietness, clock)) {
            if (!bound_sample.threads_quiet) {
              if (save_began)
                log::error("launch", "Quit file save still running after {} ms",
                           std::chrono::duration_cast<std::chrono::milliseconds>(clock - *save_began).count());
              log::error("launch", "OS quit save barrier not ready: {}", menu_quiet_error);
            }
            if (!bound_sample.threads_quiet || !bound_sample.writers_known) os_quit_failed(unsettled);
            os_quit_failed(std::format("pause/stop/destroy or save quietness not established: {} (stages {:#x} of 0xf{}, "
                                       "native destroys {}/{})", unsettled, menu_progress.stages,
                                       menu_progress.valid ? "" : ", progress unknown", os_quit::native_destroy_count.load(),
                                       menu_progress.native_activities));
          }
          if (menu_writers == platform::DataWriters::open)
            log::warn("launch", "OS quit bound reached with an idle open application data writer; terminating as a Quest would");
          end_os_menu_quit(*files, *execution);
        }
        if (host_stop && shutdown_trace::enabled) {
          shutdown_trace::snapshot("backstop");
          profile::shutdown_burst();
        }
        if (os_quit::enabled && host_stop) {
          log::error("launch", "Host quit backstop expired; ending worker without guest thread teardown (P1)");
          shutdown_trace::phase("host-quit-backstop");
          // The +1/+3 s samples can precede a completed subsystem drain.
          // Keep the actual bound's thread table with the P1 exit evidence.
          end_guest_process_after_grace();
        }
        log::warn("launch", "Lifecycle teardown is still waiting; interrupting guest execution");
        interrupt_guest_process_after_grace();
        interrupted = true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  });
  log::info("launch", "Running game Application/Activity on Android main thread");
  const bool launched = run_app_main(application_path, &error);
  shutdown_trace::phase("android-main-returned");
  monitor.request_stop();
  monitor.join();
  // A completed host-quit Looper must not fall into Shutdown's thread/arena
  // drain either. Actual guest exit already takes the terminal worker path.
  if (os_quit::enabled && host_stop) {
    if (!launched) record_guest_process_failure(1);
    const auto status = guest_process_exit_status();
    exit_guest_process(status < 0 ? 0 : status);
  }
  shutdown_trace::phase("native-activities-destroy");
  destroy_native_activities();
  shutdown_trace::phase("native-activities-destroyed");
  if (!launched) throw std::runtime_error(error);
  const auto status = guest_process_exit_status();
  // An exit the host did not request, before any XR frame, leaves the launcher
  // nothing to show: report it and never return success for it.
  if (!host_stop && !frame_loop_started.load(std::memory_order_relaxed)) {
    const auto last = hle::libc::last_guest_error();
    log::error("runtime", "The game exited before its first XR frame ({}){}{}",
               status >= 0 ? std::format("exit code {}", status) : std::string("main loop ended"),
               last.empty() ? "" : "; last game error: ", last);
    throw ProcessExit(status > 0 ? status : 1);
  }
  if (status >= 0) throw ProcessExit(status);
  log::info("launch", "Android main loop exited");
}
}
