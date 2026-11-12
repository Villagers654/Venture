// XR_EXT_debug_utils without the host: messengers, object names and session
// labels are accepted and validated. A guest callback is never called from a
// host thread; messages the game submits go to Venture's log instead.
#include "common/log.h"
#include "hle/openxr/state.h"
#include <format>
#include <string>
#include <utility>

namespace venture::hle::openxr::guest {
namespace {
std::string_view text(const char* value) { return value ? std::string_view(value) : std::string_view("-"); }
const char* severity_name(XrDebugUtilsMessageSeverityFlagsEXT severity) {
  if (severity & XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) return "error";
  if (severity & XR_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) return "warning";
  if (severity & XR_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) return "info";
  return "verbose";
}
constexpr XrDebugUtilsMessageSeverityFlagsEXT kSeverities =
    XR_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
    XR_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
constexpr XrDebugUtilsMessageTypeFlagsEXT kTypes =
    XR_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
    XR_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_TYPE_CONFORMANCE_BIT_EXT;
XrResult label_session(XrSession session, const XrDebugUtilsLabelEXT* label, const char* what) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(session, "XrSession")) return XR_ERROR_HANDLE_INVALID;
  if (!enabled("XR_EXT_debug_utils")) return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (!label || label->type != XR_TYPE_DEBUG_UTILS_LABEL_EXT || !label->labelName) return XR_ERROR_VALIDATION_FAILURE;
  log::debug("xr", "Guest debug label {} {}", what, label->labelName);
  return XR_SUCCESS;
}
}  // namespace

XrResult xrCreateDebugUtilsMessengerEXT(XrInstance instance, const XrDebugUtilsMessengerCreateInfoEXT* createInfo,
                                        XrDebugUtilsMessengerEXT* messenger) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(instance, "XrInstance")) return XR_ERROR_HANDLE_INVALID;
  if (!enabled("XR_EXT_debug_utils")) return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (!messenger || !createInfo || createInfo->type != XR_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT ||
      !createInfo->userCallback || !createInfo->messageSeverities || (createInfo->messageSeverities & ~kSeverities) ||
      !createInfo->messageTypes || (createInfo->messageTypes & ~kTypes))
    return XR_ERROR_VALIDATION_FAILURE;
  // Guest-only handles, numbered apart from host handles of other types.
  static std::uint64_t next = 0x7d500000;
  *messenger = reinterpret_cast<XrDebugUtilsMessengerEXT>(++next);
  track_handle(*messenger, "XrDebugUtilsMessengerEXT", instance);
  static bool noted = false;
  if (!std::exchange(noted, true))
    log::info("xr", "Guest debug messenger accepted; submitted messages go to this log");
  return XR_SUCCESS;
}
XrResult xrDestroyDebugUtilsMessengerEXT(XrDebugUtilsMessengerEXT messenger) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(messenger, "XrDebugUtilsMessengerEXT")) return XR_ERROR_HANDLE_INVALID;
  if (!enabled("XR_EXT_debug_utils")) return XR_ERROR_FUNCTION_UNSUPPORTED;
  forget_handle(messenger);
  return XR_SUCCESS;
}
XrResult xrSubmitDebugUtilsMessageEXT(XrInstance instance, XrDebugUtilsMessageSeverityFlagsEXT messageSeverity,
                                      XrDebugUtilsMessageTypeFlagsEXT messageTypes,
                                      const XrDebugUtilsMessengerCallbackDataEXT* callbackData) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(instance, "XrInstance")) return XR_ERROR_HANDLE_INVALID;
  if (!enabled("XR_EXT_debug_utils")) return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (!callbackData || callbackData->type != XR_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT ||
      !messageSeverity || (messageSeverity & ~kSeverities) || !messageTypes || (messageTypes & ~kTypes) ||
      (callbackData->objectCount && !callbackData->objects) ||
      (callbackData->sessionLabelCount && !callbackData->sessionLabels))
    return XR_ERROR_VALIDATION_FAILURE;
  const auto line = std::format("Guest debug message ({}) id={} function={}: {}", severity_name(messageSeverity),
                                text(callbackData->messageId), text(callbackData->functionName),
                                text(callbackData->message));
  if (messageSeverity & XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) log::error("xr", "{}", line);
  else if (messageSeverity & XR_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) log::warn("xr", "{}", line);
  else if (messageSeverity & XR_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) log::info("xr", "{}", line);
  else log::debug("xr", "{}", line);
  return XR_SUCCESS;
}
XrResult xrSetDebugUtilsObjectNameEXT(XrInstance instance, const XrDebugUtilsObjectNameInfoEXT* nameInfo) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(instance, "XrInstance")) return XR_ERROR_HANDLE_INVALID;
  if (!enabled("XR_EXT_debug_utils")) return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (!nameInfo || nameInfo->type != XR_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT ||
      nameInfo->objectType == XR_OBJECT_TYPE_UNKNOWN || !nameInfo->objectHandle)
    return XR_ERROR_VALIDATION_FAILURE;
  log::debug("xr", "Guest names object type {} {:#x}: {}", int(nameInfo->objectType), nameInfo->objectHandle,
             text(nameInfo->objectName));
  return XR_SUCCESS;
}
XrResult xrSessionBeginDebugUtilsLabelRegionEXT(XrSession session, const XrDebugUtilsLabelEXT* labelInfo) {
  return label_session(session, labelInfo, "begin");
}
XrResult xrSessionEndDebugUtilsLabelRegionEXT(XrSession session) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(session, "XrSession")) return XR_ERROR_HANDLE_INVALID;
  if (!enabled("XR_EXT_debug_utils")) return XR_ERROR_FUNCTION_UNSUPPORTED;
  return XR_SUCCESS;
}
XrResult xrSessionInsertDebugUtilsLabelEXT(XrSession session, const XrDebugUtilsLabelEXT* labelInfo) {
  return label_session(session, labelInfo, "insert");
}
}  // namespace venture::hle::openxr::guest
