// SPDX-License-Identifier: GPL-3.0-or-later
// Play on an Android headset (launcher/headset_play.h): probe the headset,
// plan, adapt the game's APK, sign and install it through the app's Java
// Installer, copy its expansion files, record it for the Platform broker and
// start it.
#include "launcher/headset_play.h"

#include "launcher/headset_check.h"
#include "launcher/host_features.h"

#include "common/android/apk_rewrite.h"
#include "common/android/game_adapter.h"
#include "common/android/headset_games.h"
#include "common/download/manager.h"
#include "common/json.h"
#include "common/log.h"
#include "common/paths.h"
#include "common/platform/android/jni.h"

#define XR_USE_PLATFORM_ANDROID
#include <dlfcn.h>
#include <jni.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>

namespace venture::launcher::headset {
namespace jni = venture::android;
namespace nh = venture::android::native_host;
namespace {
// Changes whenever Venture's adaptation of a game changes, so a new Venture
// reinstalls the games it adapted with the older one.
constexpr std::string_view kAdaptation = VENTURE_VERSION;

jmethodID method(JNIEnv* env, const char* name, const char* signature) {
  if (!jni::ready() || !jni::classes().installer) throw std::runtime_error("Venture's installer is unavailable.");
  jmethodID id = env->GetStaticMethodID(jni::classes().installer, name, signature);
  if (!id) {
    jni::take_exception(env);
    throw std::runtime_error("Venture's installer is unavailable.");
  }
  return id;
}

// An Installer call returning null on success or a sentence for the user.
std::string call(const char* name, const char* signature, auto... arguments) {
  JNIEnv* env = jni::env();
  jni::LocalFrame frame(env);
  auto* result = static_cast<jstring>(
      env->CallStaticObjectMethod(jni::classes().installer, method(env, name, signature), arguments...));
  if (auto error = jni::take_exception(env); !error.empty()) return "Venture's installer failed: " + error;
  return result ? jni::utf8(env, result) : std::string{};
}
void check(const std::string& error) {
  if (!error.empty()) throw std::runtime_error(error);
}
jstring java_path(JNIEnv* env, const fs::path& path) { return jni::java_string(env, paths::utf8(path)); }
jstring text(JNIEnv* env, std::string_view value) { return jni::java_string(env, value); }

// The OpenXR runtime's version and extensions, through the app's own copy of
// the Khronos loader (lib/arm64-v8a/libopenxr_loader.so).
void probe_xr(nh::HostProfile& host, std::string& runtime) {
  void* loader = dlopen("libopenxr_loader.so", RTLD_NOW | RTLD_LOCAL);
  if (!loader) {
    log::warn("headset", "the Khronos loader did not load: {}", dlerror());
    return;
  }
  auto get = reinterpret_cast<PFN_xrGetInstanceProcAddr>(dlsym(loader, "xrGetInstanceProcAddr"));
  PFN_xrInitializeLoaderKHR initialize = nullptr;
  PFN_xrEnumerateInstanceExtensionProperties enumerate = nullptr;
  PFN_xrCreateInstance create = nullptr;
  if (get) {
    get(XR_NULL_HANDLE, "xrInitializeLoaderKHR", reinterpret_cast<PFN_xrVoidFunction*>(&initialize));
    get(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", reinterpret_cast<PFN_xrVoidFunction*>(&enumerate));
    get(XR_NULL_HANDLE, "xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction*>(&create));
  }
  XrLoaderInitInfoAndroidKHR init{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR, nullptr, jni::vm(), jni::context()};
  if (!initialize || !enumerate || !create ||
      XR_FAILED(initialize(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&init)))) {
    log::warn("headset", "the Khronos loader could not start");
    return;  // the loader stays loaded: it keeps process-wide state
  }
  std::uint32_t count = 0;
  if (XR_FAILED(enumerate(nullptr, 0, &count, nullptr))) {
    log::warn("headset", "no OpenXR runtime is active on this headset");
    return;
  }
  std::vector<XrExtensionProperties> extensions(count, {XR_TYPE_EXTENSION_PROPERTIES});
  enumerate(nullptr, count, &count, extensions.data());
  for (std::uint32_t i = 0; i < count; ++i)
    host.xr_extensions.emplace(extensions[i].extensionName, extensions[i].extensionVersion);
  // The highest version the runtime accepts: an instance asking for 1.1.
  const bool android_instance = host.xr_extensions.contains("XR_KHR_android_create_instance");
  const char* enabled[] = {"XR_KHR_android_create_instance"};
  XrInstanceCreateInfoAndroidKHR android{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR, nullptr, jni::vm(),
                                         jni::context()};
  XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
  if (android_instance) {
    info.next = &android;
    info.enabledExtensionCount = 1;
    info.enabledExtensionNames = enabled;
  }
  std::strncpy(info.applicationInfo.applicationName, "Venture headset check", XR_MAX_APPLICATION_NAME_SIZE - 1);
  info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 1, 0);
  XrInstance instance = XR_NULL_HANDLE;
  XrResult result = create(&info, &instance);
  host.xr_api_major = 1;
  host.xr_api_minor = XR_SUCCEEDED(result) ? 1 : 0;
  if (XR_FAILED(result)) {
    if (result != XR_ERROR_API_VERSION_UNSUPPORTED)
      log::warn("headset", "an OpenXR 1.1 instance failed with {}; assuming 1.0", static_cast<int>(result));
    // A 1.0 instance, for the runtime's name in the headset check.
    info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    if (XR_FAILED(create(&info, &instance))) instance = XR_NULL_HANDLE;
  }
  if (instance != XR_NULL_HANDLE) {
    PFN_xrGetInstanceProperties properties = nullptr;
    get(instance, "xrGetInstanceProperties", reinterpret_cast<PFN_xrVoidFunction*>(&properties));
    XrInstanceProperties about{XR_TYPE_INSTANCE_PROPERTIES};
    if (properties && XR_SUCCEEDED(properties(instance, &about)))
      runtime = std::format("{} {}.{}.{}", about.runtimeName, XR_VERSION_MAJOR(about.runtimeVersion),
                            XR_VERSION_MINOR(about.runtimeVersion), XR_VERSION_PATCH(about.runtimeVersion));
    PFN_xrDestroyInstance destroy = nullptr;
    get(instance, "xrDestroyInstance", reinterpret_cast<PFN_xrVoidFunction*>(&destroy));
    if (destroy) destroy(instance);
  }
}

struct Probe {
  nh::HostProfile host;
  HostFacts facts;
};
fs::path g_runtime_file;  // use_runtime

// The runtime use_runtime names, in place of the device's.
void replace_runtime(nh::HostProfile& host, std::string& runtime) {
  const auto json = paths::read_json(g_runtime_file);
  runtime = str(json, "runtime") + " (from " + paths::utf8(g_runtime_file.filename()) + ")";
  host.xr_api_major = 1;
  host.xr_api_minor = static_cast<std::int32_t>(number(json, "api_minor"));
  host.xr_extensions.clear();
  if (auto it = json.find("extensions"); it != json.end() && it->is_object())
    for (const auto& [name, version] : it->items())
      if (version.is_number_unsigned()) host.xr_extensions.emplace(name, version.get<std::uint32_t>());
  log::warn("headset", "the OpenXR runtime is {}, not the device's", runtime);
}

Probe probe() {
  Probe out;
  auto& host = out.host;
  auto& facts = out.facts;
  auto json = parse_json(call("probe", "()Ljava/lang/String;"));
  // The device's ABIs, preferred first. A 32-bit ABI counts only when the
  // device lists it among its 32-bit ones (Build.SUPPORTED_32_BIT_ABIS): then
  // an ARM32 game runs natively, with Venture's 32-bit libraries.
  std::vector<std::string> abis32;
  if (auto it = json.find("abis32"); it != json.end() && it->is_array())
    for (const auto& abi : *it)
      if (abi.is_string()) abis32.push_back(abi.get<std::string>());
  if (auto it = json.find("abis"); it != json.end() && it->is_array())
    for (const auto& abi : *it) {
      if (!abi.is_string()) continue;
      auto name = abi.get<std::string>();
      const bool bits32 = name == "armeabi-v7a" || name == "armeabi" || name == "x86";
      if (!bits32 || std::ranges::find(abis32, name) != abis32.end()) host.abis.push_back(std::move(name));
    }
  facts.abis32 = abis32;
  facts.device = str(json, "device");
  facts.install_permission = field(json, "installPermission") == true;
  facts.obb_access = field(json, "obbAccess") == true;
  facts.page_size = sysconf(_SC_PAGESIZE);
  host.android_api = static_cast<std::int32_t>(number(json, "sdk"));
  host.platform_service = field(json, "platformService") == true;
  if (auto it = json.find("launchCategories"); it != json.end() && it->is_array())
    for (const auto& c : *it)
      if (c.is_string()) host.launch_categories.push_back(c.get<std::string>());
  if (auto it = json.find("launchMetadata"); it != json.end() && it->is_array())
    for (const auto& pair : *it)
      if (pair.is_array() && pair.size() == 2 && pair[0].is_string() && pair[1].is_string())
        host.launch_metadata.emplace_back(pair[0].get<std::string>(), pair[1].get<std::string>());
  // A VrApi implementation is a system library any app may load.
  if (void* vrapi = dlopen("libvrapi.so", RTLD_NOW | RTLD_LOCAL)) {
    host.vrapi = true;
    dlclose(vrapi);
  }
  if (g_runtime_file.empty())
    probe_xr(host, facts.runtime);
  else
    replace_runtime(host, facts.runtime);
  // What the runtime brokers name for each ABI: a 32-bit game needs one for its ABI.
  const auto rows = parse_json(call("runtimes", "()Ljava/lang/String;"));
  for (const auto& row : rows.is_array() ? rows : Json::array())
    if (row.is_object())
      facts.runtimes.push_back({str(row, "broker"), str(row, "abi"), str(row, "package"), str(row, "library"),
                                field(row, "present") == true});
  std::string abis;
  for (const auto& abi : host.abis) abis += (abis.empty() ? "" : ",") + abi;
  log::info("headset", "probed: API {}, ABIs {}, OpenXR 1.{} with {} extensions, VrApi {}, Platform service {}",
            host.android_api, abis, host.xr_api_minor, host.xr_extensions.size(), host.vrapi, host.platform_service);
  return out;
}
const Probe& probed() {
  static std::once_flag once;
  static Probe value;
  std::call_once(once, [] { value = probe(); });
  return value;
}

const meta::File* first(const meta::Plan& plan, std::string_view kind) {
  auto it = std::ranges::find(plan.files, kind, &meta::File::kind);
  return it == plan.files.end() ? nullptr : &*it;
}

// <data>/headset/adapted/<package>: the binary and Venture version the
// installed copy was adapted from.
fs::path stamp_file(const fs::path& data, std::string_view package) {
  if (!paths::safe_name(package)) throw std::runtime_error("The game's package name is not valid.");
  return data / "headset" / "adapted" / paths::from_utf8(package);
}
std::string stamp(const meta::Plan& plan) { return plan.binary_id + " " + std::string(kAdaptation); }
std::string read_text(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}
std::int64_t version_code(const meta::Plan& plan) {
  try {
    return std::stoll(plan.version_code);
  } catch (...) {
    return 0;
  }
}

// The headset's copy of a game: its version code (-1: not installed), whether
// the game key signed it, and whether it was adapted from this version by this
// Venture.
struct Installed {
  std::int64_t version = -1;
  bool ours = false, current = false;
};
Installed installed(const meta::Plan& plan, const fs::path& data) {
  JNIEnv* env = jni::env();
  jni::LocalFrame frame(env);
  auto package = text(env, plan.package);
  Installed out;
  out.version = env->CallStaticLongMethod(jni::classes().installer,
                                          method(env, "installedVersion", "(Ljava/lang/String;)J"), package);
  out.ours = env->CallStaticBooleanMethod(jni::classes().installer,
                                          method(env, "signedByUs", "(Ljava/lang/String;)Z"), package);
  check(jni::take_exception(env));
  out.current = out.version == version_code(plan) && out.ours && read_text(stamp_file(data, plan.package)) == stamp(plan);
  return out;
}

// Copies the files that exist into Android/obb/<package> (Installer.copyObbs).
void copy_to_obb(const std::string& package, const std::vector<fs::path>& files) {
  std::vector<std::string> present;
  for (const auto& path : files)
    if (std::error_code ec; fs::is_regular_file(path, ec)) present.push_back(paths::utf8(path));
  if (present.empty()) return;
  JNIEnv* env = jni::env();
  jni::LocalFrame frame(env, int(present.size()) + 8);
  auto array = env->NewObjectArray(jsize(present.size()), env->FindClass("java/lang/String"), nullptr);
  for (std::size_t i = 0; i < present.size(); ++i)
    env->SetObjectArrayElement(array, jsize(i), text(env, present[i]));
  check(call("copyObbs", "(Ljava/lang/String;[Ljava/lang/String;)Ljava/lang/String;", text(env, package), array));
}

struct AssetRoute {
  fs::path install;
  std::function<Json(const Json&)> handler;
};
std::mutex g_routes_mutex;
std::map<std::string, AssetRoute, std::less<>> g_routes;
}  // namespace

void serve_assets(std::string package, fs::path install, std::function<Json(const Json&)> handler) {
  std::lock_guard lock(g_routes_mutex);
  g_routes[std::move(package)] = {std::move(install), std::move(handler)};
}

std::optional<Json> asset_request(std::string_view package, const Json& request) {
  AssetRoute route;
  {
    std::lock_guard lock(g_routes_mutex);
    auto it = g_routes.find(package);
    if (it == g_routes.end()) return std::nullopt;
    route = it->second;
  }
  auto reply = route.handler(request);
  // The download lands in Venture's copy of the game; the game reads its OBB folder.
  const auto op = str(request, "op"), name = str(request, "name");
  try {
    if (op == "asset.status" && field(reply, "ok") == true && field(reply, "state").is_number_integer() &&
        reply["state"].get<int>() == static_cast<int>(download::State::complete))
      copy_to_obb(std::string(package), {route.install / "plan.json", route.install / paths::from_utf8(name)});
    else if (op == "asset.delete" && field(reply, "success") == true) {
      JNIEnv* env = jni::env();
      jni::LocalFrame frame(env);
      check(call("removeObb", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", text(env, package),
                 text(env, name)));
      copy_to_obb(std::string(package), {route.install / "plan.json"});
    }
  } catch (const std::exception& e) {
    log::warn("headset", "could not update the OBB folder of {}: {}", package, e.what());
    return Json{{"ok", false}};
  }
  return reply;
}

const nh::HostProfile& host() { return probed().host; }
}  // namespace venture::launcher::headset

namespace venture::launcher {
// Games here are the device's own apps (play() below).
HostFeatures host_features() { return device_app_features(); }
}  // namespace venture::launcher

namespace venture::launcher::headset {
void use_runtime(const fs::path& file) { g_runtime_file = file; }

void play(const library::Install& install, const std::string& account, const fs::path& data,
          std::function<Json(const Json&)> assets, std::stop_token stop,
          const std::function<void(std::string)>& status) {
  const auto& plan = install.plan;
  const auto* apk_file = first(plan, "apk");
  if (!apk_file) throw std::runtime_error("This game's install has no APK.");
  const auto apk = install.directory / paths::from_utf8(apk_file->name);
  const std::int64_t version = version_code(plan);
  // The Platform broker serves only the games recorded here, for this account.
  android::record_headset_game(data, {plan.package, plan.app_id, plan.binary_id, version, plan.version, account});

  const auto stamp_path = stamp_file(data, plan.package);
  JNIEnv* env = jni::env();
  const bool current = installed(plan, data).current;

  if (!current) {
    status("Preparing the game for this headset\xE2\x80\xA6");
    const auto profile = nh::profile(apk);
    const auto adapted = nh::plan(profile, host());
    if (!adapted.blockers.empty()) throw std::runtime_error(adapted.blockers.front());
    // An ARM32 game runs natively where the CPU has a 32-bit mode (the plan
    // then picks armeabi-v7a); only a CPU without one is left.
    if (adapted.execution != nh::Execution::native)
      throw std::runtime_error(
          "This game ships only 32-bit ARM code, and this headset's CPU has no 32-bit mode to run it. Venture for "
          "headsets cannot translate it yet.");
    const auto work = jni::app().cache / "headset" / paths::from_utf8(plan.package);
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(work);
    struct Cleanup {
      fs::path path;
      ~Cleanup() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
      }
    } cleanup{work};
    const auto edit = android::adapt_game(apk, adapted, jni::app().apk, work / "parts");
    const auto unsigned_apk = work / "unsigned.apk", signed_apk = work / "game.apk";
    android::rewrite_apk(apk, edit, unsigned_apk, stop);
    if (stop.stop_requested()) throw std::runtime_error("Cancelled.");
    status("Signing the game\xE2\x80\xA6");
    {
      jni::LocalFrame frame(env);
      check(call("sign", "(Ljava/lang/String;Ljava/lang/String;I)Ljava/lang/String;", java_path(env, unsigned_apk),
                 java_path(env, signed_apk), jint(std::max(profile.min_sdk, 24))));
    }
    fs::remove(unsigned_apk, ec);
    status("Installing the game\xE2\x80\xA6 Confirm the install when Android asks.");
    {
      jni::LocalFrame frame(env);
      check(call("install", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", java_path(env, signed_apk),
                 text(env, plan.package)));
    }
    fs::create_directories(stamp_path.parent_path());
    paths::atomic_write(stamp_path, stamp(plan));
    log::info("headset", "installed {} {} ({} replacements, layer {})", plan.package, plan.version,
              adapted.replace.size(), adapted.quest_layer);
  }

  // Expansion and asset files go to Android/obb/<package>, where the game
  // reads them, with their catalog (plan.json) for the Platform loader's
  // asset file calls. This comes after the install: on Android 11 and later
  // Venture may write that folder once the user allowed it to install apps,
  // which the install asks for; Android 10 asks for storage access here
  // (Installer.obbAccess).
  std::vector<fs::path> files{install.directory / "plan.json"};
  for (const auto& f : plan.files)
    if (f.kind == "obb" || f.kind == "asset") files.push_back(install.directory / paths::from_utf8(f.name));
  status("Copying the game's expansion files\xE2\x80\xA6");
  copy_to_obb(plan.package, files);
  // Asset downloads the game asks for while it runs.
  if (assets) serve_assets(plan.package, install.directory, std::move(assets));

  status("Starting the game\xE2\x80\xA6");
  jni::LocalFrame frame(env);
  check(call("launch", "(Ljava/lang/String;)Ljava/lang/String;", text(env, plan.package)));
}

Check check(const std::vector<library::Install>& installs,
            const std::function<std::string(const library::Install&)>& name, std::string_view package,
            const fs::path& data) {
  Check out;
  try {
    const auto& p = probed();
    out.text = describe_host(p.host, p.facts);
    for (const auto& install : installs) {
      GameFacts game{.name = name(install), .package = install.plan.package, .version = install.plan.version};
      try {
        if (const auto* apk = first(install.plan, "apk"))
          game.apk = nh::profile(install.directory / paths::from_utf8(apk->name));
        else
          game.apk_error = "the install has no APK";
      } catch (const std::exception& e) {
        game.apk_error = e.what();
      }
      try {
        const auto state = installed(install.plan, data);
        game.installed_version = state.version;
        game.signed_by_venture = state.ours;
        game.adapted_current = state.current;
      } catch (const std::exception& e) {
        game.apk_error += (game.apk_error.empty() ? "" : "; ") + std::string(e.what());
      }
      out.text += describe_game(game, p.host);
    }
    if (installs.empty()) out.text += "\nNo games installed in Venture.\n";
    // The logs the Venture libraries in the game wrote (android_game/game_log.h,
    // and the Quest layer's quest_layer/records.h).
    if (!package.empty() && paths::safe_name(package)) {
      JNIEnv* env = jni::env();
      jni::LocalFrame frame(env);
      const auto folder = paths::from_utf8(call("obbPath","(Ljava/lang/String;)Ljava/lang/String;",
                                                text(env, std::string(package)))) / "venture";
      for (const char* file : {"vrapi.log", "platform.log", "questlayer.log", "vrapi.prev.log", "platform.prev.log",
                               "questlayer.prev.log"})
        if (std::error_code ec; fs::is_regular_file(folder / file, ec)) out.logs.push_back(folder / file);
    }
  } catch (const std::exception& e) {
    out.text += std::format("The headset check stopped: {}\n", e.what());
  }
  return out;
}

void update_self(const fs::path& apk) {
  JNIEnv* env = jni::env();
  jni::LocalFrame frame(env);
  jobject context = jni::context();
  jclass cls = env->GetObjectClass(context);
  jmethodID name = env->GetMethodID(cls, "getPackageName", "()Ljava/lang/String;");
  auto package = name ? static_cast<jstring>(env->CallObjectMethod(context, name)) : nullptr;
  check(jni::take_exception(env));
  if (!package) throw std::runtime_error("Could not install the update.");
  check(call("install", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", java_path(env, apk), package));
}
}  // namespace venture::launcher::headset
