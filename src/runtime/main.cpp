#include "common/environment.h"
#include "common/log.h"
#include "common/paths.h"
#include "common/launch_details.h"
#include "core/process_image.h"
#include "core/engine_defaults.h"
#include "core/renderer_capabilities.h"
#include "core/diagnostic_trace.h"
#include "common/meta/accounts.h"
#include "hle/ovrplatform/broker.h"
#include <charconv>
#include "core/linker_runtime.h"
#include "core/process_runtime.h"
#include "core/thread_runtime.h"
#include "core/libc_lifecycle.h"
#include "core/elf.h"
#include "core/a32_coverage.h"
#include "core/address_space.h"
#include "core/profile.h"
#include "platform/runtime_qos.h"
#include "core/guest_storage.h"
#include "platform/low_memory.h"
#include "platform/crash_report.h"
#include "platform/runtime_child.h"
#include "platform/runtime_stop.h"
#include "common/process.h"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include "hle/registry.h"
#include "hle/libandroid/libandroid.h"
#include "hle/ovrplatform/hle.h"
#include "hle/steam/library.h"
#include "hle/steam/bridge.h"
#include "hle/vulkan/vulkan.h"
#include "audio/audio.h"
#include "core/process_exit.h"
#include "hle/vulkan/timing.h"
#include "gfx/dispatch.h"
#include "java/jvm.h"
#include "java/conversion.h"
#include "java/app_runtime.h"
#include "java/support_files.h"
#include "java/natives/audio_bridge.h"
#include "java/natives/horizon_bridge.h"
#include "hle/openxr/openxr.h"
#include "hle/vrapi/vrapi.h"
#include "hle/vrapi/state.h"
#include <jni.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::unique_ptr<venture::ovrplatform::Session> register_libraries(const std::filesystem::path& game,
    std::shared_ptr<venture::PipeChannel> broker = {}) {
  venture::AddressSpace::get().initialize(venture::process_config().isa());
  const auto& config = venture::process_config();
  if (config.source == "steam") {
    auto plan=venture::steam::parse_plan(venture::paths::read_json(game / "plan.json"));
    if (plan.package!=config.package || plan.version_code!=config.version_code)
      throw std::runtime_error("Steam launch plan does not match configured APK identity");
    venture::steam::register_library(plan, config.apk);
  }
  venture::register_all_hle_libraries();
  venture::hle::register_libandroid();
  venture::hle::register_libmediandk();
  venture::hle::register_libcamera2ndk();
  venture::audio::register_libraries();
  venture::gfx::register_graphics_libraries();
  venture::vulkan::register_library();
  venture::hle::openxr::register_library();
  venture::hle::vrapi::register_libraries();
  if (config.source == "steam") { venture::process_image::publish(); return {}; }
  venture::ovrplatform::AppIdentity identity;
  identity.package = config.package;
  identity.version_code = config.version_code;
  if (!game.empty()) identity = venture::ovrplatform::read_app_identity(game / "plan.json", config.package, config.version_code);
  auto session = venture::ovrplatform::make_runtime_session(identity, game, broker);
  venture::ovrplatform::register_library(*session);
  // app_process, the linker and system libraries as tools on a device find them.
  venture::process_image::publish();
  // Standalone NDK fixtures have only their JNI test classes on the classpath;
  // the Horizon OS supplement belongs to an installed Android application's
  // framework, not that minimal JVM.
  if (!game.empty()) venture::java::install_horizon_bridge(*session);
  return session;
}
struct Shutdown {
  venture::GuestThread& thread;
  ~Shutdown() {
    venture::shutdown_trace::phase("host-audio-drain");
    venture::java::shutdown_audio_tracks();
    venture::audio::shutdown_libraries();
    venture::shutdown_trace::phase("host-guest-drain");
    venture::shutdown_guest_threads();
    if (venture::process_config().source == "steam") venture::steam::Bridge::get().shutdown();
    // A failed boot must not run destructors in partially initialized libraries.
    thread.exit_requested = true;
    if (venture::java::host_vm()) {
      venture::shutdown_trace::phase("host-jvm-drain");
      std::string error;
      if (!venture::java::stop_jvm(&error)) venture::log::error("java", "shutdown: {}", error);
    }
    venture::shutdown_trace::phase("host-drained");
    venture::shutdown_trace::stop();
  }
};
// Library finalizers run after the guest threads have stopped. One that waits
// for its own worker thread (an instrumentation runtime's agent thread, say)
// would wait forever. Android kills an app process without running them, so
// past this bound the process ends with its status, as there.
class FinalizerBound {
 public:
  explicit FinalizerBound(int status) : watchdog_([this, status] {
    std::unique_lock lock(mutex_);
    if (done_.wait_for(lock, std::chrono::seconds(5), [this] { return finished_; })) return;
    venture::log::warn("runtime", "guest library finalizers did not finish within 5 s; ending the process as Android would");
    const int recorded = venture::guest_process_exit_status();
    const int final_status = recorded >= 0 ? recorded : status;
    venture::flush_guest_process_files(final_status);
    venture::platform::terminate_runtime_process(final_status);
  }) {}
  ~FinalizerBound() {
    { std::lock_guard lock(mutex_); finished_ = true; }
    done_.notify_all();
    watchdog_.join();
  }
 private:
  std::mutex mutex_;
  std::condition_variable done_;
  bool finished_ = false;
  std::thread watchdog_;
};
void finalize(venture::GuestThread& thread, int status = 0) {
  venture::shutdown_trace::phase("finalize-audio");
  venture::java::shutdown_audio_tracks();
  venture::audio::shutdown_libraries();
  venture::shutdown_trace::phase("finalize-guests");
  venture::shutdown_guest_threads();
  venture::finish_guest_thread(thread);
  venture::run_guest_exit_handlers(thread);
  venture::shutdown_trace::phase("finalize-linker");
  {
    FinalizerBound bound(status);
    venture::linker_finalize();
  }
  venture::shutdown_trace::phase("finalized");
}
jobject start_java(const std::filesystem::path& home, std::vector<std::string> class_path, bool required) {
  if (!std::filesystem::exists(home / "include/jni.h") && !std::filesystem::exists(home / "lib/modules")) {
    if (required) throw std::runtime_error("JDK absent; run python tools/bootstrap.py --jdk");
    venture::log::warn("runtime", "JDK absent; native boot will stop after constructors");
    return nullptr;
  }
  const auto support = venture::java::support_files();
  class_path.insert(class_path.begin(), venture::paths::utf8(support.bridge));
  // Diagnostic and NDK-fixture JVMs need the same Android classes as app
  // launches: installed native registrars also bind framework facades.
  class_path.insert(class_path.begin(), venture::paths::utf8(support.framework));
  venture::java::JvmOptions options{venture::paths::utf8(home), std::move(class_path), {"-Xcheck:jni", "-Xverify:all"}};
  options.agent_path = venture::paths::utf8(support.agent);
  std::string error;
  if (!venture::java::start_jvm(options, &error)) throw std::runtime_error(error);
  auto* env = venture::java::host_env();
  auto type = env->FindClass("java/lang/ClassLoader");
  auto method = type ? env->GetStaticMethodID(type, "getSystemClassLoader", "()Ljava/lang/ClassLoader;") : nullptr;
  auto loader = method ? env->CallStaticObjectMethod(type, method) : nullptr;
  if (env->ExceptionCheck() || !loader) throw std::runtime_error("cannot create application class loader");
  venture::java::set_app_class_loader(loader);
  env->DeleteLocalRef(type);
  return loader;
}
}  // namespace

int main(int argc, char** argv) {
  if (auto status = venture::platform::run_runtime_dump_helper(argc, argv))
    venture::platform::terminate_runtime_process(*status);
  venture::platform::pre_reserve_low_memory();
  struct ProfileExit { ~ProfileExit() { venture::profile::stop(); } } profile_exit;
  try {
    const auto process_began = std::chrono::steady_clock::now();
    const auto support = venture::java::support_files();
    std::filesystem::path game, guest_test, test_dir, java_home = support.home;
    std::vector<std::string> class_path;
    std::string package, logfile;
    std::string account_key;
    std::uintptr_t credential_read = 0, credential_write = 0;
    bool inspect = false, boot_native = false, defer_gpu = false, test_a32_memory = false;
    bool leaderboard_writes = true;
    bool runtime_worker = false, console = false;
    std::uint32_t parent_pid = 0;
    double run_seconds = 0;
    for (int i = 1; i < argc; ++i) {
      std::string option = argv[i];
      auto value = [&]() -> std::string { if (++i == argc) throw std::runtime_error("missing value for " + option); return argv[i]; };
      if (option == "--game") game = venture::paths::from_utf8(value());
      else if (option == "--package") package = value();
      else if (option == "--log") logfile = value();
      else if (option == "--console") console = true;
      else if (option == "--account") {
        account_key = value();
        // "local": no account; the game's platform calls are unauthenticated.
        if (account_key != "local" && !venture::meta::valid_account_key(account_key))
          throw std::runtime_error(
              "Invalid account key (expected 12 lowercase hex characters or local)");
      } else if (option == "--shared-from")
        throw std::runtime_error(
            "App Sharing is not supported; each launching "
            "account must own the game");
      else if (option == "--credential-read" ||
               option == "--credential-write") {
        auto text = value();
        std::uintptr_t handle = 0;
        auto [end, error] =
            std::from_chars(text.data(), text.data() + text.size(), handle);
        if (error != std::errc{} || end != text.data() + text.size() || !handle)
          throw std::runtime_error("Invalid credential pipe handle");
        (option == "--credential-read" ? credential_read : credential_write) =
            handle;
      }
      else if (option == "--inspect") inspect = true;
      else if (option == "--boot-native") boot_native = true;
      else if (option == "--defer-gpu") defer_gpu = true;
      else if (option == "--no-leaderboard-writes") leaderboard_writes = false;
      else if (option == "--test-a32-memory")
        test_a32_memory = true;
      else if (option == "--run-guest-test") guest_test = venture::paths::from_utf8(value());
      else if (option == "--test-dir") test_dir = venture::paths::from_utf8(value());
      else if (option == "--java-home") java_home = venture::paths::from_utf8(value());
      else if (option == "--class-path") class_path.push_back(value());
      else if (option == "--parent-pid") parent_pid = static_cast<std::uint32_t>(std::stoul(value()));
      else if (option == "--run-seconds") run_seconds = std::stod(value());
      else if (option == "--runtime-worker") runtime_worker = true;
      else if (option == "--runtime-restart-write") {
        if (!runtime_worker) throw std::runtime_error("Restart channel requires an app worker");
        venture::platform::set_runtime_restart_channel(std::stoull(value()));
      }
      else if (option == "--runtime-launch-read") {
        if (!runtime_worker) throw std::runtime_error("Launch channel requires an app worker");
        venture::platform::set_runtime_launch_channel(std::stoull(value()));
      }
      else if (option == "--runtime-teardown-write") {
        if (!runtime_worker) throw std::runtime_error("Lifecycle acknowledgement requires an app worker");
        venture::platform::set_runtime_teardown_channel(std::stoull(value()));
      }
      else if (option == "--help") {
        std::cout << "venture-runtime --game <install dir> [--boot-native | "
                     "--inspect] [--package <pkg>] [--log <file>] [--console]\n"
                     "venture-runtime --run-guest-test <so> [--test-dir "
                     "<dir>] [--log <file>] [--console]\n"
                     "Java options: --java-home <JDK/JRE> --class-path <jar or "
                     "directory> (repeatable)\n";
        std::cout << "Lifecycle options: --parent-pid <launcher PID> "
                     "--run-seconds <seconds>\n"
                     "Account selection: --account <12-hex-key> (defaults to "
                     "active account)\n"
                     "Platform option: --no-leaderboard-writes (disable score submissions for this launch)\n"
                     "Diagnostic option: --defer-gpu (initialize graphics on "
                     "first guest use)\n"
                     "Memory test: --test-a32-memory (reserve low arena and "
                     "exit before HLE)\n";
        return 0;
      } else throw std::runtime_error("unknown option: " + option);
    }
    if (console) venture::log::attach_console();
    if (!logfile.empty()) venture::log::set_file(logfile);
    venture::platform::install_crash_report(logfile, package);
    // The launcher forwards its log level: 0 (trace) to 4 (error), as venture::log::Level.
    if (const char* level = venture::environment::getenv("VENTURE_LOG_LEVEL"); level && *level) {
      if (level[0] >= '0' && level[0] <= '4' && !level[1])
        venture::log::set_level(static_cast<venture::log::Level>(level[0] - '0'));
      else
        venture::log::warn("runtime", "Ignoring VENTURE_LOG_LEVEL={} (expected 0 to 4)", level);
    }
    venture::log::info("launch", "Process started");
    if (bool(credential_read) != bool(credential_write))
      throw std::runtime_error("Both credential channel handles are required");
    if (!runtime_worker && !game.empty() && guest_test.empty() && !inspect && !boot_native) {
      // Android launches a replacement process when an app queues a cold task
      // and exits. Keep this small supervisor outside the JVM/guest/GPU so a
      // fresh worker gets fresh native globals and the old commit is released.
      venture::platform::release_pre_reserved_low_memory();
      venture::platform::RuntimeStop stop(parent_pid);
      const auto began = std::chrono::steady_clock::now();
      auto expired = [&] {
        return run_seconds > 0 && std::chrono::duration<double>(
            std::chrono::steady_clock::now() - began).count() >= run_seconds;
      };
      std::vector<std::string> arguments{"--runtime-worker"};
      for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--run-seconds" || option == "--parent-pid") { ++i; continue; }
        arguments.emplace_back(option);
      }
      arguments.insert(arguments.end(), {"--parent-pid", std::to_string(venture::current_process_id())});
      const std::vector<std::uintptr_t> handles = credential_read
          ? std::vector<std::uintptr_t>{credential_read, credential_write} : std::vector<std::uintptr_t>{};
      std::string launch_payload;
      // A log file alone is not permission to capture stack memory. Only an
      // explicit harness/dev run directory enables private timeout evidence.
      const auto* diagnostic_run = venture::environment::getenv("VENTURE_DIAGNOSTIC_RUN_DIR");
      const auto run_directory = diagnostic_run
          ? venture::paths::from_utf8(diagnostic_run) : std::filesystem::path{};
      const auto diagnostics = venture::platform::runtime_dump_directory(run_directory);
      for (;;) {
        if (stop.requested() || expired()) return 0;
        auto attempt = arguments;
        if (run_seconds > 0) attempt.insert(attempt.end(), {"--run-seconds", std::to_string(
            run_seconds - std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count())});
        auto result = venture::platform::run_runtime_child(venture::paths::executable(), attempt, handles, [&] {
          return stop.requested() || (run_seconds > 0 && std::chrono::duration<double>(
              std::chrono::steady_clock::now() - began).count() >= run_seconds + 10);
        }, launch_payload, true, run_directory);
        if ((result.status == 124 || result.os_termination) && !diagnostics.empty()) {
          try {
            venture::paths::atomic_write(diagnostics / "worker-exit.json", venture::Json{
                {"exit_code", result.status}, {"reason", result.os_termination ? "os_task_removal" :
                    result.supervisor_stopped ? "supervisor_teardown_bound" : "worker_exit"},
                {"os_task_removal", result.os_termination},
                {"timeout_dump", venture::paths::utf8(result.timeout_dump.filename())},
                {"dump_error", result.dump_error},
                {"exit_wait", venture::platform::runtime_exit_wait_name(result.exit_wait)},
                {"exit_wait_tid", result.exit_wait_tid}}.dump());
          } catch (const std::exception& error) {
            venture::log::warn("launch", "Cannot record worker timeout evidence: {}", error.what());
          }
        }
        if (result.supervisor_stopped)
          venture::log::warn("launch", "Stopping app worker after shutdown request or teardown grace expired");
        if (const auto status = result.completion_status(stop.requested() || expired()))
          return *status;
        launch_payload = std::move(result.launch_payload);
        venture::log::info("launch", "Application requested a fresh process; relaunching within the original run bound");
      }
    }
    venture::platform::initialize_runtime_qos();
    venture::profile::start();
    auto broker = credential_read ? venture::PipeChannel::inherited(credential_read,
                                                               credential_write)
                                  : nullptr;
    // Temporary memory-packet diagnostic until the ELF/APK ISA selector lands.
    // Reserves at startup, before any HLE registration or dynamic host library.
    if (test_a32_memory) {
      venture::process_config().abi = "armeabi-v7a";
      venture::AddressSpace::get().initialize(venture::GuestIsa::a32);
      return 0;
    }
    if (!guest_test.empty() && !game.empty()) throw std::runtime_error("choose --game or --run-guest-test");
    if (inspect && boot_native) throw std::runtime_error("choose --inspect or --boot-native");
    if (!guest_test.empty()) {
      auto& config = venture::process_config();
      config.package = "org.venture.guesttests";
      venture::select_process_isa(venture::elf::detect_isa(guest_test));
      config.native_lib_dir = std::filesystem::absolute(guest_test).parent_path();
      config.data_dir = test_dir.empty() ? std::filesystem::current_path() / "guest-test-data" : std::filesystem::absolute(test_dir);
      config.cache_dir = config.data_dir / "cache";
      config.guest_cache_dir = config.data_dir / "cache";
      std::filesystem::create_directories(config.cache_dir);
      venture::freeze_process_isa();
      auto platform_session = register_libraries({});
      venture::Linker::get().set_search_paths({venture::paths::utf8(config.native_lib_dir)});
      venture::linker_set_inspection(inspect);
      if (inspect && config.isa() == venture::GuestIsa::a32) venture::hle::a32::install_import_coverage_hook();
      if (inspect) {
        if (!venture::Linker::get().open(venture::paths::utf8(std::filesystem::absolute(guest_test)), venture::kRtldNow | venture::kRtldGlobal, nullptr))
          throw std::runtime_error(venture::Linker::get().take_error());
        venture::write_inspection_report(config.cache_dir / "inspect.json");
        return 0;
      }
      venture::GuestThread thread(1); thread.attach_to_current_host_thread();
      Shutdown shutdown{thread};
      auto* library = venture::Linker::get().open(venture::paths::utf8(std::filesystem::absolute(guest_test)), venture::kRtldNow | venture::kRtldGlobal, nullptr);
      if (!library) throw std::runtime_error(venture::Linker::get().take_error());
      if (library->find_symbol("JNI_OnLoad")) {
        if (class_path.empty()) class_path.emplace_back(venture::paths::utf8(venture::paths::executable().parent_path() / "guest/java-classes"));
        auto loader = start_java(java_home, class_path, true);
        std::string error;
        if (!venture::java::load_guest_library(library->path(), loader, &error)) {
          if (venture::java::host_env()->ExceptionCheck()) venture::java::host_env()->ExceptionDescribe();
          throw std::runtime_error(error);
        }
      }
      auto entry = library->find_symbol("vtest_main");
      if (!entry) throw std::runtime_error("guest test does not export vtest_main");
      const auto filename = venture::paths::utf8(guest_test.filename());
      const auto guest_dir = config.guest_data_dir();
      const char* arguments[]{filename.c_str(), guest_dir.c_str(), nullptr};
      venture::GuestStorage arguments32;
      auto argv_address = venture::h2g(arguments);
      if (config.isa() == venture::GuestIsa::a32) {
        const std::uint32_t words[]{venture::h2g32(venture::low_string(filename)), venture::h2g32(venture::low_string(guest_dir)), 0};
        argv_address = arguments32.resize(sizeof(words));
        std::memcpy(venture::g2h(argv_address), words, sizeof(words));
      }
      auto result = thread.call(entry, {2, argv_address});
      finalize(thread, static_cast<int>(result));
      venture::log::info("runtime", "guest test returned {}", result);
      return static_cast<int>(result);
    }
    if (game.empty()) throw std::runtime_error("--game or --run-guest-test is required (see --help)");
    venture::log::info("launch", "Configuring installed game {}", venture::paths::utf8(game));
    const auto index = venture::meta::AccountVault(venture::paths::data_dir()).index();
    if (account_key.empty()) account_key = index.active;
    if (!account_key.empty() && account_key != "local") {
      auto account = index.find(account_key);
      if (!account || !account->signed_in || (broker && account->needs_reauth))
        throw std::runtime_error(
            "Reconnect the selected account in the "
            "launcher before starting a game");
    }
    venture::initialize_diagnostic_trace();
    venture::configure_game(game, package,
                       account_key.empty() ? "local" : account_key);
    venture::AddressSpace::get().initialize(venture::process_config().isa());
    venture::log::info("launch", "Validating cached {} libraries", venture::process_config().abi);
    auto libraries = venture::extract_native_libraries();
    if (libraries.empty()) throw std::runtime_error("no native libraries in the selected APK slice");
    if (!inspect) venture::engine_policy::prepare_renderer_defaults(venture::process_config(), libraries,
        {venture::renderer_capabilities::vrapi_variable_rate_shading});
    venture::process_config().platform_leaderboard_writes = leaderboard_writes;
    venture::freeze_process_isa();
    if (broker && account_key.empty())
      throw std::runtime_error("Credential broker requires an account key");
    auto platform_session = register_libraries(game, broker);
    if (venture::startup_diagnostics::enabled) {
      venture::startup_diagnostics::set_sources([platform = platform_session.get()] {
        if (platform) platform->log_startup_snapshot();
        {
          auto& s = venture::xr::session();
          std::unique_lock lock(s.mutex, std::try_to_lock);
          if (!lock.owns_lock()) venture::log::info("startup-snapshot", "xr lock_available=false");
          else venture::log::info("startup-snapshot", "xr lock_available=true owner={} state={} running={} phase={} wait_in_progress={} pending_wait={} frames={} errors={} epoch={}",
              static_cast<int>(s.owner), static_cast<int>(s.state), s.running, static_cast<int>(s.phase),
              s.wait_in_progress, s.pending_wait, s.frame_index, s.end_errors, s.frame_epoch);
        }
        {
          auto& s = venture::hle::vrapi::state();
          std::unique_lock lock(s.mutex, std::try_to_lock);
          if (!lock.owns_lock()) venture::log::info("startup-snapshot", "vrapi lock_available=false");
          else venture::log::info("startup-snapshot", "vrapi lock_available=true initialized={} mobile={} token={} frame_bound={} wait_in_progress={} frames={} next_frame={}",
              s.initialized, s.mobile != nullptr, s.token, s.frame_bound, s.wait_in_progress, s.frame_index, s.next_frame_index);
        }
      });
    }
    struct ClearStartupSources { ~ClearStartupSources() { venture::startup_diagnostics::set_sources({}); } } clear_startup_sources;
    if (broker && platform_session) {
      const auto frame = broker->call(venture::Json{{"op", "launch_details"}});
      platform_session->set_launch_details(venture::read_launch_details(frame.at("details")));
      platform_session->attach_launcher(broker);
    }
    venture::Linker::get().set_search_paths({venture::paths::utf8(venture::process_config().native_lib_dir)});
    venture::linker_set_inspection(inspect);
    if (inspect && venture::process_config().isa() == venture::GuestIsa::a32) venture::hle::a32::install_import_coverage_hook();
    if (inspect) {
      for (const auto& path : libraries) if (!venture::Linker::get().open(venture::paths::utf8(path.filename()), venture::kRtldNow | venture::kRtldGlobal, nullptr))
        throw std::runtime_error(venture::Linker::get().take_error());
      venture::write_inspection_report(venture::process_config().cache_dir / "inspect.json");
      return 0;
    }
    venture::GuestThread thread(1); thread.attach_to_current_host_thread();
    Shutdown shutdown{thread};
    venture::set_terminal_guest_process_exit(runtime_worker && !boot_native);
    venture::set_guest_exit_capture_flush([] {
      venture::audio::Backend::get().finish_capture();
      venture::platform::notify_runtime_exit_wait(venture::platform::RuntimeExitWait::gpu_telemetry_flush,
                                                   venture::guest_process_exit_status());
      venture::vulkan::finish_timing();
      venture::xr::finish_runtime_diagnostics();
    });
    // Android app_process has a global platform libc++ dependency (AOSP
    // cmds/app_process/Android.bp and build/soong/cc/stl.go). Plugins can use
    // its C++ ABI without their own DT_NEEDED entry, as RE4's OVRPlugin does.
    // Use the APK's guest runtime for that role, including RTTI and unwinding;
    // ordinary JNI libraries still load RTLD_LOCAL. Never use host C++ objects.
    if (std::filesystem::is_regular_file(venture::process_config().native_lib_dir / "libc++_shared.so")) {
      venture::log::info("launch", "Loading guest C++ runtime into the process scope");
      if (!venture::Linker::get().open("libc++_shared.so", venture::kRtldNow | venture::kRtldGlobal | venture::kRtldNoDelete, nullptr))
        throw std::runtime_error(venture::Linker::get().take_error());
    }
    if (!boot_native) {
      // A restart must not reset the launch's time limit. Account for worker
      // startup before the Android lifecycle monitor begins.
      if (runtime_worker && run_seconds > 0) run_seconds = std::max(0.001, run_seconds -
          std::chrono::duration<double>(std::chrono::steady_clock::now() - process_began).count());
      venture::java::launch_android_app({java_home, venture::paths::executable().parent_path(),
                                    class_path, parent_pid, run_seconds, defer_gpu});
      finalize(thread);
      return 0;
    }
    if (class_path.empty() && std::filesystem::exists(java_home / "lib/modules")) {
      venture::java::ConversionOptions options;
      options.java = venture::paths::utf8(support.dex_java);
      options.tools = venture::paths::utf8(support.dex_tools);
      options.apk = venture::paths::utf8(venture::process_config().apk);
      options.package = venture::process_config().package;
      options.cache = venture::paths::utf8(venture::process_config().cache_dir / "java");
      options.framework_jars = {venture::paths::utf8(support.framework), venture::paths::utf8(support.bridge)};
      options.hierarchy_metadata = {venture::paths::utf8(support.hierarchy)};
      venture::java::ConvertedApk converted;
      std::string error;
      const bool verified = venture::java::convert_apk(options, &converted, &error);
      if (!std::filesystem::is_regular_file(venture::paths::from_utf8(converted.app_jar))) throw std::runtime_error(error);
      if (!verified) venture::log::warn("boot",
            "APK verification is incomplete; diagnostic native boot loads "
            "individual classes under HotSpot verification. Report: {}", converted.report);
      class_path.push_back(converted.app_jar);
    }
    start_java(java_home, class_path, false);
    const auto& directory = venture::process_config().native_lib_dir;
    std::vector<std::string> engines;
    if (std::filesystem::exists(directory / "libunity.so")) engines = {"libmain.so", "libunity.so", "libil2cpp.so"};
    else if (std::filesystem::exists(directory / "libUE4.so")) engines = {"libUE4.so"};
    else throw std::runtime_error("no supported Unity/Unreal engine found");
    std::vector<venture::Library*> loaded;
    for (const auto& name : engines) {
      if (!std::filesystem::exists(directory / name)) {
        if (name == "libil2cpp.so") continue; // Mono Unity does not include IL2CPP.
        throw std::runtime_error("engine library absent: " + name);
      }
      venture::log::info("boot", "{}: loading {} and running constructors", venture::process_config().package, name);
      auto* lib = venture::Linker::get().open(name, venture::kRtldNow | venture::kRtldGlobal, nullptr);
      if (!lib) throw std::runtime_error(venture::Linker::get().take_error());
      loaded.push_back(lib);
      venture::log::info("boot", "{}: constructors complete", name);
    }
    if (venture::java::host_vm()) {
      for (auto* lib : loaded) if (auto onload = lib->find_symbol("JNI_OnLoad")) {
        venture::log::info("boot", "{}: calling JNI_OnLoad", lib->soname());
        const auto result = static_cast<std::int32_t>(thread.call(onload, {venture::java::guest_vm(), 0}));
        venture::log::info("boot", "{}: JNI_OnLoad returned {:#x}", lib->soname(), result);
        auto* env = venture::java::host_env();
        if (env->ExceptionCheck()) { env->ExceptionDescribe(); throw std::runtime_error(lib->soname() + ": JNI_OnLoad left a Java exception"); }
        if (result != JNI_VERSION_1_2 && result != JNI_VERSION_1_4 && result != JNI_VERSION_1_6)
          throw std::runtime_error(lib->soname() + ": JNI_OnLoad failed");
      }
    }
    venture::log::info("boot", "{}: native boot complete (constructors{}, no Activity/XR startup)",
                  venture::process_config().package, venture::java::host_vm() ? " and JNI_OnLoad" : " only");
    finalize(thread);
    return 0;
  } catch (const venture::ProcessExit& e) { return e.status; }
  catch (const venture::ThreadExit& e) { return static_cast<int>(e.value); }
  catch (const std::exception& e) { venture::log::error("runtime", "{}", e.what()); return 1; }
}
