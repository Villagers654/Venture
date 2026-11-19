// The Quest compatibility layer between a Quest-style OpenXR 1.1 application
// and the mock runtime wearing one headset's capability profile
// (VENTURE_MOCK_HEADSET; each profile is its own ctest entry). The application
// goes through the real Khronos loader with the layer enabled from its
// manifest, as an adapted game does on the headset.
#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "common/json.h"
#include "common/quest_xr_surface.h"
#include "headsets.h"
#include "quest_layer/layer.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

using namespace venture;

namespace {
// An elevated process (GitHub's Windows runners are) makes the OpenXR loader
// ignore XR_RUNTIME_JSON and XR_API_LAYER_PATH, so the mock runtime and the
// layer under test can't be selected there.
bool loader_ignores_test_environment() {
#ifdef _WIN32
  HANDLE token = nullptr;
  TOKEN_ELEVATION elevation{};
  DWORD size = 0;
  const bool elevated = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) &&
                        GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) &&
                        elevation.TokenIsElevated;
  if (token) CloseHandle(token);
  if (elevated) MESSAGE("skipped: the OpenXR loader ignores XR_RUNTIME_JSON in an elevated process");
  return elevated;
#else
  return false;
#endif
}
const mock::HeadsetProfile& profile() {
  const char* name = std::getenv("VENTURE_MOCK_HEADSET");
  const auto* found = name ? mock::headset_profile(name) : nullptr;
  REQUIRE_MESSAGE(found, "VENTURE_MOCK_HEADSET names no profile");
  return *found;
}
bool profile_has(std::string_view extension) {
  return std::ranges::find(profile().extensions, extension) != profile().extensions.end();
}
bool runtime_lacks_local_floor() {
  const char* absent = std::getenv("VENTURE_MOCK_NO_EXTENSIONS");
  return absent && std::string_view(absent).find("XR_EXT_local_floor") != std::string_view::npos;
}

// The layer library's report (export.cpp), from the copy the loader loaded.
std::string layer_report() {
  using Report = std::size_t (*)(char*, std::size_t);
#ifdef _WIN32
  auto* module = GetModuleHandleA(VENTURE_QUEST_LAYER_FILE);
  REQUIRE(module);
  auto report = reinterpret_cast<Report>(GetProcAddress(module, "ventureQuestLayerReport"));
#else
  void* module = dlopen(VENTURE_QUEST_LAYER_FILE, RTLD_NOW | RTLD_NOLOAD);
  REQUIRE(module);
  auto report = reinterpret_cast<Report>(dlsym(module, "ventureQuestLayerReport"));
  dlclose(module);
#endif
  REQUIRE(report);
  std::string text(report(nullptr, 0), '\0');
  report(text.data(), text.size() + 1);
  return text;
}

// The layer's records (quest_layer/records.h), at VENTURE_QUEST_LAYER_LOG.
std::string layer_records() {
  const char* path = std::getenv("VENTURE_QUEST_LAYER_LOG");
  REQUIRE_MESSAGE(path, "VENTURE_QUEST_LAYER_LOG is not set");
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}
bool recorded(std::string_view text) { return layer_records().find(text) != std::string::npos; }

template <typename F>
F function(XrInstance instance, const char* name) {
  PFN_xrVoidFunction pointer = nullptr;
  if (XR_FAILED(xrGetInstanceProcAddr(instance, name, &pointer))) return nullptr;
  return reinterpret_cast<F>(pointer);
}

bool same_pose(const XrPosef& a, const XrPosef& b) {
  const auto close = [](float x, float y) { return std::fabs(x - y) < 1e-5f; };
  return close(a.position.x, b.position.x) && close(a.position.y, b.position.y) && close(a.position.z, b.position.z) &&
         close(a.orientation.x, b.orientation.x) && close(a.orientation.y, b.orientation.y) &&
         close(a.orientation.z, b.orientation.z) && close(a.orientation.w, b.orientation.w);
}
}  // namespace

TEST_CASE("the layer answers Quest extensions at the versions Quest games are built against") {
  for (const auto& extension : quest_layer::emulated_extensions()) {
    CAPTURE(extension.name);
    const auto* quest = quest_xr::find(extension.name);
    REQUIRE(quest);
    CHECK(quest->kind == quest_xr::Kind::emulated);
    CHECK(quest->version == extension.version);
  }
}

TEST_CASE("the layer's manifests list exactly the extensions it answers") {
  for (const auto* path : {VENTURE_QUEST_LAYER_MANIFEST, VENTURE_QUEST_LAYER_IMPLICIT_MANIFEST}) {
    CAPTURE(path);
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in);
    const auto manifest = parse_json(std::string(std::istreambuf_iterator<char>(in), {}));
    const auto& layer = manifest.at("api_layer");
    CHECK(layer.at("name") == VENTURE_QUEST_LAYER_NAME);
    std::set<std::pair<std::string, std::string>> listed, answered;
    for (const auto& extension : layer.at("instance_extensions"))
      listed.emplace(extension.at("name"), extension.at("extension_version"));
    for (const auto& extension : quest_layer::emulated_extensions())
      answered.emplace(extension.name, std::to_string(extension.version));
    CHECK(listed == answered);
  }
}

TEST_CASE("a Quest OpenXR 1.1 game runs on this headset's runtime through the layer") {
  if (loader_ignores_test_environment()) return;
  const auto& headset = profile();
  CAPTURE(headset.name);
  const bool lowered = headset.api_minor == 0;

  // The game asks for 1.1 and every Quest extension the layer can answer.
  std::vector<const char*> extensions{XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
  std::set<std::string> expected_emulated;
  for (const auto& extension : quest_layer::emulated_extensions()) {
    extensions.push_back(extension.name);
    if (!profile_has(extension.name)) expected_emulated.emplace(extension.name);
  }
  const char* layers[] = {VENTURE_QUEST_LAYER_NAME};
  XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
  std::strcpy(create.applicationInfo.applicationName, "Quest layer test");
  create.applicationInfo.apiVersion = XR_API_VERSION_1_1;
  create.enabledApiLayerCount = 1;
  create.enabledApiLayerNames = layers;
  create.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
  create.enabledExtensionNames = extensions.data();
  XrInstance instance = XR_NULL_HANDLE;
  REQUIRE(xrCreateInstance(&create, &instance) == XR_SUCCESS);

  std::string emulated;
  for (const auto& name : expected_emulated) emulated += (emulated.empty() ? "" : ",") + name;
  const auto report = layer_report();
  CHECK(report == "lowered=" + std::string(lowered ? "1" : "0") + ";emulated=" + emulated + ";refresh_fallbacks=0");
  // The records name the game, what it asked for and what the layer did.
  CHECK(recorded("instance: Quest layer test 0, engine  0, OpenXR 1.1." +
                 std::to_string(XR_VERSION_PATCH(XR_API_VERSION_1_1)) + "; extensions: XR_KHR_vulkan_enable2, "));
  CHECK(recorded("created on "));
  std::string answered;
  for (const auto& name : expected_emulated) answered += (answered.empty() ? "" : ", ") + name;
  CHECK(recorded("; answered here: " + (answered.empty() ? std::string("none") : answered)));
  if (lowered) CHECK(recorded("; at OpenXR 1.0, the runtime's version"));

  XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
  system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  XrSystemId system = XR_NULL_SYSTEM_ID;
  REQUIRE(xrGetSystem(instance, &system_info, &system) == XR_SUCCESS);
  // The mock binds no Vulkan objects without a Vulkan instance.
  XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
  binding.device = reinterpret_cast<VkDevice>(std::uintptr_t{1});
  XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO, &binding, 0, system};
  XrSession session = XR_NULL_HANDLE;
  REQUIRE(xrCreateSession(instance, &session_info, &session) == XR_SUCCESS);
  for (int i = 0; i < 100; ++i) {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    if (xrPollEvent(instance, &event) != XR_SUCCESS) continue;
    if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED &&
        reinterpret_cast<XrEventDataSessionStateChanged&>(event).state == XR_SESSION_STATE_READY)
      break;
  }
  XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
  REQUIRE(xrBeginSession(session, &begin) == XR_SUCCESS);
  XrFrameState frame{XR_TYPE_FRAME_STATE};
  REQUIRE(xrWaitFrame(session, nullptr, &frame) == XR_SUCCESS);
  REQUIRE(xrBeginFrame(session, nullptr) == XR_SUCCESS);
  XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO, nullptr, frame.predictedDisplayTime, XR_ENVIRONMENT_BLEND_MODE_OPAQUE};
  REQUIRE(xrEndFrame(session, &end) == XR_SUCCESS);
  // More frames for the session's frame timing.
  for (int i = 0; i < 20; ++i) {
    XrFrameState state{XR_TYPE_FRAME_STATE};
    REQUIRE(xrWaitFrame(session, nullptr, &state) == XR_SUCCESS);
    REQUIRE(xrBeginFrame(session, nullptr) == XR_SUCCESS);
    XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO, nullptr, state.predictedDisplayTime, XR_ENVIRONMENT_BLEND_MODE_OPAQUE};
    REQUIRE(xrEndFrame(session, &info) == XR_SUCCESS);
  }

  SUBCASE("refresh rates") {
    const auto list = function<PFN_xrEnumerateDisplayRefreshRatesFB>(instance, "xrEnumerateDisplayRefreshRatesFB");
    const auto get = function<PFN_xrGetDisplayRefreshRateFB>(instance, "xrGetDisplayRefreshRateFB");
    const auto request = function<PFN_xrRequestDisplayRefreshRateFB>(instance, "xrRequestDisplayRefreshRateFB");
    REQUIRE(list);
    REQUIRE(get);
    REQUIRE(request);
    std::uint32_t count = 0;
    REQUIRE(list(session, 0, &count, nullptr) == XR_SUCCESS);
    std::vector<float> rates(count);
    REQUIRE(list(session, count, &count, rates.data()) == XR_SUCCESS);
    REQUIRE(!rates.empty());
    if (expected_emulated.contains("XR_FB_display_refresh_rate")) {
      // The display rate the runtime reports through xrWaitFrame.
      const float hz = std::round(1e11f / static_cast<float>(frame.predictedDisplayPeriod)) / 100.0f;
      CHECK(rates == std::vector<float>{hz});
    } else {
      CHECK(std::ranges::equal(rates, headset.refresh_rates));
    }
    // Every listed rate is granted, as on Quest.
    for (const float rate : rates) CHECK(request(session, rate) == XR_SUCCESS);
    CHECK(request(session, 0.0f) == XR_SUCCESS);
    CHECK(request(session, 61.0f) == XR_ERROR_DISPLAY_REFRESH_RATE_UNSUPPORTED_FB);
    // An error a game gets is recorded with the function's name.
    CHECK(recorded("xrRequestDisplayRefreshRateFB returned XR_ERROR_DISPLAY_REFRESH_RATE_UNSUPPORTED_FB"));
    if (headset.refuses_refresh_requests)
      CHECK(recorded("the runtime lists the rate but returned XR_ERROR_DISPLAY_REFRESH_RATE_UNSUPPORTED_FB"));
    float current = 0;
    CHECK(get(session, &current) == XR_SUCCESS);
    CHECK(std::ranges::find(rates, current) != rates.end());
    const auto fallbacks = headset.refuses_refresh_requests ? rates.size() : 0;
    CHECK(layer_report().ends_with(";refresh_fallbacks=" + std::to_string(fallbacks)));
  }

  SUBCASE("spaces") {
    XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space_info.poseInReferenceSpace.orientation.w = 1;
    XrSpace floor = XR_NULL_HANDLE, stage = XR_NULL_HANDLE, view = XR_NULL_HANDLE, local = XR_NULL_HANDLE;
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR;
    REQUIRE(xrCreateReferenceSpace(session, &space_info, &floor) == XR_SUCCESS);
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    REQUIRE(xrCreateReferenceSpace(session, &space_info, &stage) == XR_SUCCESS);
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    REQUIRE(xrCreateReferenceSpace(session, &space_info, &view) == XR_SUCCESS);
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    REQUIRE(xrCreateReferenceSpace(session, &space_info, &local) == XR_SUCCESS);

    std::uint32_t count = 0;
    REQUIRE(xrEnumerateReferenceSpaces(session, 0, &count, nullptr) == XR_SUCCESS);
    std::vector<XrReferenceSpaceType> types(count);
    REQUIRE(xrEnumerateReferenceSpaces(session, count, &count, types.data()) == XR_SUCCESS);
    CHECK(std::ranges::find(types, XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR) != types.end());

    const auto time = frame.predictedDisplayTime;
    const XrSpace spaces[] = {view, floor, stage};
    std::vector<XrSpaceLocation> single;
    for (const auto space : spaces) {
      XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
      REQUIRE(xrLocateSpace(space, local, time, &location) == XR_SUCCESS);
      single.push_back(location);
    }
    if (lowered && runtime_lacks_local_floor()) {
      CHECK(same_pose(single[1].pose, single[2].pose));
      CHECK(recorded("shim: the LOCAL_FLOOR space is the runtime's STAGE space"));
    }

    // xrLocateSpaces (1.1 core) where the layer lowered the instance, and
    // xrLocateSpacesKHR where it answers XR_KHR_locate_spaces.
    std::vector<const char*> names;
    if (lowered) names.push_back("xrLocateSpaces");
    if (expected_emulated.contains("XR_KHR_locate_spaces")) names.push_back("xrLocateSpacesKHR");
    for (const auto* name : names) {
      CAPTURE(name);
      const auto locate = function<PFN_xrLocateSpaces>(instance, name);
      REQUIRE(locate);
      XrSpaceLocationData data[3]{};
      XrSpaceVelocityData velocity[3]{};
      XrSpaceVelocities velocities{XR_TYPE_SPACE_VELOCITIES, nullptr, 3, velocity};
      XrSpaceLocations locations{XR_TYPE_SPACE_LOCATIONS, &velocities, 3, data};
      XrSpacesLocateInfo info{XR_TYPE_SPACES_LOCATE_INFO, nullptr, local, time, 3, spaces};
      REQUIRE(locate(session, &info, &locations) == XR_SUCCESS);
      for (int i = 0; i < 3; ++i) {
        CHECK(data[i].locationFlags == single[i].locationFlags);
        CHECK(same_pose(data[i].pose, single[i].pose));
      }
      locations.locationCount = 2;
      CHECK(locate(session, &info, &locations) == XR_ERROR_VALIDATION_FAILURE);
    }
    for (const auto space : {floor, stage, view, local}) CHECK(xrDestroySpace(space) == XR_SUCCESS);
  }

  SUBCASE("color space and thread hints") {
    if (expected_emulated.contains("XR_FB_color_space")) {
      const auto list = function<PFN_xrEnumerateColorSpacesFB>(instance, "xrEnumerateColorSpacesFB");
      const auto set = function<PFN_xrSetColorSpaceFB>(instance, "xrSetColorSpaceFB");
      REQUIRE(list);
      REQUIRE(set);
      std::uint32_t count = 0;
      REQUIRE(list(session, 0, &count, nullptr) == XR_SUCCESS);
      std::vector<XrColorSpaceFB> spaces(count);
      REQUIRE(list(session, count, &count, spaces.data()) == XR_SUCCESS);
      CHECK(spaces == std::vector<XrColorSpaceFB>{XR_COLOR_SPACE_UNMANAGED_FB});
      CHECK(set(session, XR_COLOR_SPACE_REC2020_FB) == XR_SUCCESS);
    }
    if (expected_emulated.contains("XR_KHR_android_thread_settings")) {
      using SetThread = XrResult(XRAPI_PTR*)(XrSession, std::int32_t, std::uint32_t);
      const auto set_thread = function<SetThread>(instance, "xrSetAndroidApplicationThreadKHR");
      REQUIRE(set_thread);
      CHECK(set_thread(session, 1, 1234) == XR_SUCCESS);
    }
  }

  CHECK(xrEndSession(session) != XR_ERROR_HANDLE_INVALID);
  CHECK(xrDestroySession(session) == XR_SUCCESS);
  // The session's frame timing, written when it ends.
  CHECK(recorded("frame timing, session 1 ended: 21 frames in "));
  CHECK(recorded(" fps average; frame time p95 "));
  CHECK(xrDestroyInstance(instance) == XR_SUCCESS);
}

TEST_CASE("a game that asks for OpenXR 1.0 gets a 1.0 instance, not a lowered one") {
  if (loader_ignores_test_environment()) return;
  const char* layers[] = {VENTURE_QUEST_LAYER_NAME};
  XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
  std::strcpy(create.applicationInfo.applicationName, "Quest layer test");
  create.applicationInfo.apiVersion = XR_API_VERSION_1_0;
  create.enabledApiLayerCount = 1;
  create.enabledApiLayerNames = layers;
  XrInstance instance = XR_NULL_HANDLE;
  REQUIRE(xrCreateInstance(&create, &instance) == XR_SUCCESS);
  CHECK(layer_report() == "lowered=0;emulated=;refresh_fallbacks=0");
  CHECK(xrDestroyInstance(instance) == XR_SUCCESS);
}

TEST_CASE("without the layer, the profile's runtime accepts only its own OpenXR version") {
  if (loader_ignores_test_environment()) return;
  XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
  std::strcpy(create.applicationInfo.applicationName, "Quest layer test");
  create.applicationInfo.apiVersion = XR_API_VERSION_1_1;
  XrInstance instance = XR_NULL_HANDLE;
  const auto result = xrCreateInstance(&create, &instance);
  if (profile().api_minor == 0) {
    CHECK(result == XR_ERROR_API_VERSION_UNSUPPORTED);
  } else {
    REQUIRE(result == XR_SUCCESS);
    CHECK(xrDestroyInstance(instance) == XR_SUCCESS);
  }
}
