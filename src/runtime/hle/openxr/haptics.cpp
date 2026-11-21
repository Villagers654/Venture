#include "hle/openxr/state.h"
#include "hle/openxr/controller_layer.h"
#include "xr/haptic_output.h"
#include "xr/haptic_sequence.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <tuple>
#include <utility>
namespace venture::hle::openxr::guest {
namespace {
XrResult reject(std::string_view api, XrSession handle, const XrHapticActionInfo *info,
                XrResult result, std::string_view call, bool safe = true) {
  XrHapticActionInfo empty{XR_TYPE_HAPTIC_ACTION_INFO};
  venture::xr::haptic_event(api, handle, info ? *info : empty, 0, 0, 0, call, result,
      {{"stage", "guest_request"}, {"host_query_safe", safe}, {"action_info_present", info != nullptr}});
  return result;
}
XrResult check(XrSession handle, const XrHapticActionInfo *info) {
  if (!valid_handle(handle, "XrSession")) return XR_ERROR_HANDLE_INVALID;
  if (!info || info->type != XR_TYPE_HAPTIC_ACTION_INFO)
    return XR_ERROR_VALIDATION_FAILURE;
  if (!valid_handle(info->action, "XrAction")) return XR_ERROR_HANDLE_INVALID;
  return XR_SUCCESS;
}
XrResult check_emulated(XrSession handle, const XrHapticActionInfo &info) {
  auto a = state().actions.find(info.action);
  if (a == state().actions.end()) return XR_ERROR_HANDLE_INVALID;
  if (a->second.type != XR_ACTION_TYPE_VIBRATION_OUTPUT) return XR_ERROR_ACTION_TYPE_MISMATCH;
  const auto paths = a->second.haptic_subactions();
  if (info.subactionPath && std::find(paths.begin(), paths.end(), info.subactionPath) == paths.end())
    return XR_ERROR_PATH_UNSUPPORTED;
  if (!state().attached_sets[handle].contains(state().objects.at({"XrAction", key(info.action)}).parent))
    return XR_ERROR_ACTIONSET_NOT_ATTACHED;
  if (session().state != XR_SESSION_STATE_FOCUSED) return XR_SESSION_NOT_FOCUSED;
  return XR_SUCCESS;
}
// A host can expose XR_FB_haptic_pcm and still answer a sample rate of 0 for the bound
// device: it cannot play PCM there, though core vibration may work (Monado answers 0 for
// every controller whose driver reports no output limits). Those actions take the core
// sequence emulation that hosts without the extension use. Queries refresh the answer;
// a title that never queries is asked once per action and hand.
std::map<std::tuple<XrSession, XrAction, XrPath>, bool> host_pcm_silent;
bool pcm_silent_on_host(XrSession handle, const XrHapticActionInfo &info) {
  auto key = std::tuple{handle, info.action, info.subactionPath};
  auto it = host_pcm_silent.find(key);
  if (it == host_pcm_silent.end()) {
    XrDevicePcmSampleRateGetInfoFB rate{XR_TYPE_DEVICE_PCM_SAMPLE_RATE_STATE_FB};
    auto result = host_call<PFN_xrGetDeviceSampleRateFB, "xrGetDeviceSampleRateFB">(handle, &info, &rate);
    it = host_pcm_silent.emplace(key, XR_SUCCEEDED(result) && !(rate.sampleRate > 0)).first;
  }
  return it->second;
}
bool samples_valid(const float *data, uint32_t count, bool signed_pcm) {
  if (!data || !count || (!signed_pcm && count > 4000)) return false;
  for (uint32_t i = 0; i < count; ++i)
    if (!std::isfinite(data[i]) || (!signed_pcm && (data[i] < 0 || data[i] > 1))) return false;
  return true;
}
}
XrResult xrApplyHapticFeedback(XrSession handle, const XrHapticActionInfo *info,
                               const XrHapticBaseHeader *header) {
  std::lock_guard lock(state().mutex);
  auto valid = check(handle, info);
  auto api = header && header->type == XR_TYPE_HAPTIC_PCM_VIBRATION_FB ? "xr_pcm" :
      header && header->type == XR_TYPE_HAPTIC_AMPLITUDE_ENVELOPE_VIBRATION_FB ? "xr_envelope" : "xr_core";
  if (XR_FAILED(valid)) return reject(api, handle, info, valid, "validate_guest_action", false);
  if (!header) return reject(api, handle, info, XR_ERROR_VALIDATION_FAILURE, "validate_guest_payload");
  // Use the shared chain translator: unknown/unenabled extension nodes must
  // be ignored, not forwarded to a stricter host or rejected by emulation.
  InputChain action_chain, payload_chain;
  auto translated = action_chain.copy(info);
  if (XR_FAILED(translated)) return reject(api, handle, info, translated, "validate_guest_action_chain");
  translated = payload_chain.copy(header);
  if (XR_FAILED(translated)) return reject(api, handle, info, translated, "validate_guest_payload_chain");
  info = static_cast<const XrHapticActionInfo *>(action_chain.head());
  header = static_cast<const XrHapticBaseHeader *>(payload_chain.head());
  bool pcm = header->type == XR_TYPE_HAPTIC_PCM_VIBRATION_FB;
  bool envelope = header->type == XR_TYPE_HAPTIC_AMPLITUDE_ENVELOPE_VIBRATION_FB;
  if (header->type != XR_TYPE_HAPTIC_VIBRATION && !pcm && !envelope)
    return reject(api, handle, info, XR_ERROR_VALIDATION_FAILURE, "validate_guest_payload");
  auto extension = pcm ? "XR_FB_haptic_pcm" : "XR_FB_haptic_amplitude_envelope";
  if ((pcm || envelope) && !enabled(extension))
    return reject(api, handle, info, XR_ERROR_VALIDATION_FAILURE, "validate_guest_extension");
  // The game's action is Venture's own: its contract is checked here, and the
  // pulse goes to the host actions it is bound to (controller_layer.h).
  if (auto contract = check_emulated(handle, *info); contract != XR_SUCCESS) {
    if (pcm) {
      auto &p = *reinterpret_cast<const XrHapticPcmVibrationFB *>(header);
      if (p.samplesConsumed) *p.samplesConsumed = 0;
    }
    venture::xr::haptic_event(api, handle, *info, 0, 0, 0, "validate_guest_contract", contract, {{"stage", "guest_request"}});
    return contract;
  }
  // Host haptic calls run on the haptics worker (xr/haptic_sequence.h), never under this lock:
  // a runtime whose haptics are slow must not hold up the game's frame calls.
  const auto targets = controls::haptic_targets(*info);
  if (header->type == XR_TYPE_HAPTIC_VIBRATION) {
    // The host's answer arrives too late to return: its validation is done here.
    // Finite overdrive above 1 stays valid (apply_haptic saturates it).
    const auto &v = *reinterpret_cast<const XrHapticVibration *>(header);
    if (!std::isfinite(v.amplitude) || v.amplitude < 0 || !std::isfinite(v.frequency) || v.frequency < 0 ||
        (v.duration < 0 && v.duration != XR_MIN_HAPTIC_DURATION))
      return reject(api, handle, info, XR_ERROR_VALIDATION_FAILURE, "validate_guest_payload");
    for (const auto &target : targets)
      venture::xr::queue_vibration("xr_core", handle, target, *reinterpret_cast<const XrHapticVibration *>(header));
    return XR_SUCCESS;
  }
  float amplitude = 0;
  XrDuration duration = 0;
  nlohmann::json fields{{"payload_type", int(header->type)}, {"stage", "guest_request"}};
  if (pcm) {
    auto &p = *reinterpret_cast<const XrHapticPcmVibrationFB *>(header);
    auto rejected = [&] {
      nlohmann::json details{{"stage", "guest_request"}, {"buffer_size", p.bufferSize},
        {"sample_rate", p.sampleRate}, {"append", p.append}, {"samples_consumed", 0},
        {"buffer_present", p.buffer != nullptr}, {"consumed_present", p.samplesConsumed != nullptr}};
      if (p.samplesConsumed && p.buffer && p.bufferSize && p.bufferSize <= 4000) {
        float minimum = p.buffer[0], maximum = p.buffer[0];
        for (uint32_t i = 0; i < p.bufferSize; ++i) {
          minimum = std::min(minimum, p.buffer[i]); maximum = std::max(maximum, p.buffer[i]);
          if (!std::isfinite(p.buffer[i]) || p.buffer[i] < -1 || p.buffer[i] > 1) {
            details["invalid_sample_index"] = i; details["invalid_sample"] = p.buffer[i];
            break;
          }
        }
        details["sample_min"] = minimum; details["sample_max"] = maximum;
      }
      venture::xr::haptic_event("xr_pcm", handle, *info, 0, 0, 0, "validate_guest_buffer", XR_ERROR_VALIDATION_FAILURE,
          std::move(details));
      return XR_ERROR_VALIDATION_FAILURE;
    };
    if (!p.samplesConsumed) return rejected();
    *p.samplesConsumed = 0;
    if (!samples_valid(p.buffer, p.bufferSize, true) || !std::isfinite(p.sampleRate) ||
        p.sampleRate <= 0 || p.append > XR_TRUE ||
        double(p.bufferSize) * 1e9 / p.sampleRate > double(std::numeric_limits<XrDuration>::max()) / 2)
      return rejected();
    duration = XrDuration(double(p.bufferSize) * 1e9 / p.sampleRate);
    for (uint32_t i = 0; i < p.bufferSize; ++i) amplitude = std::max(amplitude, std::abs(p.buffer[i]));
    fields.update({{"buffer_size", p.bufferSize}, {"sample_rate", p.sampleRate}, {"append", p.append != XR_FALSE}});
  } else {
    auto &p = *reinterpret_cast<const XrHapticAmplitudeEnvelopeVibrationFB *>(header);
    if (!samples_valid(p.amplitudes, p.amplitudeCount, false) || p.duration <= 0) {
      venture::xr::haptic_event("xr_envelope", handle, *info, 0, p.duration, 0,
          "validate_guest_buffer", XR_ERROR_VALIDATION_FAILURE,
          {{"stage", "guest_request"}, {"buffer_size", p.amplitudeCount}});
      return XR_ERROR_VALIDATION_FAILURE;
    }
    duration = p.duration;
    for (uint32_t i = 0; i < p.amplitudeCount; ++i) amplitude = std::max(amplitude, p.amplitudes[i]);
  }
  if (targets.empty()) {
    // Bound to nothing: accepted and dropped, as a runtime does for an unbound output.
    if (pcm) {
      auto &p = *reinterpret_cast<const XrHapticPcmVibrationFB *>(header);
      *p.samplesConsumed = std::min(p.bufferSize, uint32_t(XR_MAX_HAPTIC_PCM_BUFFER_SIZE_FB));
    }
    return XR_SUCCESS;
  }
  XrResult result = XR_SUCCESS;
  for (const auto &target_info : targets) {
    const auto *target = &target_info;
    const bool native = host_xr().enabled_extensions.contains(extension) && !(pcm && pcm_silent_on_host(handle, *target));
    // A native clip or envelope is scaled by the player's vibration on the worker; an
    // emulated one in its pulses (apply_haptic).
    XrResult r = XR_SUCCESS;
    if (pcm) {
      auto &p = *reinterpret_cast<const XrHapticPcmVibrationFB *>(header);
      if (native)
        *p.samplesConsumed = venture::xr::queue_native_pcm(handle, *target, {p.buffer, p.bufferSize}, p.sampleRate,
                                                           p.append != XR_FALSE);
      else r = venture::xr::queue_haptic(handle, *target, {p.buffer, p.bufferSize}, p.sampleRate,
                                         p.append != XR_FALSE, *p.samplesConsumed);
      fields["samples_consumed"] = *p.samplesConsumed;
    } else {
      auto &p = *reinterpret_cast<const XrHapticAmplitudeEnvelopeVibrationFB *>(header);
      if (native) venture::xr::queue_native_envelope(handle, *target, {p.amplitudes, p.amplitudeCount}, p.duration);
      else r = venture::xr::queue_envelope(handle, *target, {p.amplitudes, p.amplitudeCount}, p.duration);
    }
    // The worker records what a native request does at the host.
    if (!native)
      venture::xr::haptic_event(pcm ? "xr_pcm" : "xr_envelope", handle, *target, amplitude, duration, 0,
          "enqueue_core_sequence", r, fields);
    if (result == XR_SUCCESS) result = r;
  }
  return result;
}
XrResult xrStopHapticFeedback(XrSession handle, const XrHapticActionInfo *info) {
  std::lock_guard lock(state().mutex);
  auto valid = check(handle, info);
  if (XR_FAILED(valid)) return reject("xr_core", handle, info, valid, "validate_guest_stop", false);
  InputChain action_chain;
  auto translated = action_chain.copy(info);
  if (XR_FAILED(translated)) return reject("xr_core", handle, info, translated, "validate_guest_action_chain");
  info = static_cast<const XrHapticActionInfo *>(action_chain.head());
  if (auto contract = check_emulated(handle, *info); contract != XR_SUCCESS) return contract;
  for (const auto &target : controls::haptic_targets(*info)) venture::xr::queue_stop("xr_core", handle, target);
  return XR_SUCCESS;
}
XrResult xrGetDeviceSampleRateFB(XrSession handle, const XrHapticActionInfo *info,
                                XrDevicePcmSampleRateGetInfoFB *out) {
  std::lock_guard lock(state().mutex);
  auto valid = check(handle, info);
  if (XR_FAILED(valid)) return reject("xr_pcm", handle, info, valid, "validate_guest_query", false);
  InputChain action_chain;
  auto translated = action_chain.copy(info);
  if (XR_FAILED(translated)) return reject("xr_pcm", handle, info, translated, "validate_guest_action_chain");
  info = static_cast<const XrHapticActionInfo *>(action_chain.head());
  if (!enabled("XR_FB_haptic_pcm")) return reject("xr_pcm", handle, info, XR_ERROR_FUNCTION_UNSUPPORTED, "validate_guest_extension");
  if (!out || out->type != XR_TYPE_DEVICE_PCM_SAMPLE_RATE_STATE_FB ||
      XR_FAILED(validate_chain(out->next, true, XR_TYPE_DEVICE_PCM_SAMPLE_RATE_STATE_FB, true)))
    return reject("xr_pcm", handle, info, XR_ERROR_VALIDATION_FAILURE, "validate_guest_query_output");
  XrResult result = XR_SUCCESS;
  const auto targets = controls::haptic_targets(*info);
  if (host_xr().enabled_extensions.contains("XR_FB_haptic_pcm") && !targets.empty()) {
    // The rate of the first host output the action reaches.
    const auto &target = targets.front();
    result = host_call<PFN_xrGetDeviceSampleRateFB, "xrGetDeviceSampleRateFB">(handle, &target, out);
    const bool silent = XR_SUCCEEDED(result) && !(out->sampleRate > 0);
    host_pcm_silent[{handle, target.action, target.subactionPath}] = silent;
    if (silent && (venture::xr::haptic_bound(handle, target) || controls::haptic_suggested(*info)))
      out->sampleRate = venture::xr::haptic_emulation_rate;
  } else {
    auto a = state().actions.find(info->action);
    if (a == state().actions.end()) return reject("xr_pcm", handle, info, XR_ERROR_HANDLE_INVALID, "validate_guest_query_action", false);
    if (a->second.type != XR_ACTION_TYPE_VIBRATION_OUTPUT) return reject("xr_pcm", handle, info, XR_ERROR_ACTION_TYPE_MISMATCH, "validate_guest_query_action");
    const auto paths = a->second.haptic_subactions();
    if (info->subactionPath && std::find(paths.begin(), paths.end(), info->subactionPath) == paths.end())
      return reject("xr_pcm", handle, info, XR_ERROR_PATH_UNSUPPORTED, "validate_guest_query_path");
    // The rate is a controller property on a headset, known before focus: the Haptics SDK reads it once at
    // startup (Batman) and keeps it. A suggested haptic binding is enough; pulses before the host binds are no-ops.
    bool bound = false;
    for (const auto &target : targets) bound |= venture::xr::haptic_bound(handle, target);
    out->sampleRate = controls::haptic_suggested(*info) || bound ? venture::xr::haptic_emulation_rate : 0;
  }
  // Titles poll this every frame. Record a query when its answer changes; the
  // guest API mutex protects the last answers.
  const auto rate = XR_SUCCEEDED(result) ? out->sampleRate : 0;
  static std::map<std::tuple<XrSession, XrAction, XrPath>, std::pair<XrResult, float>> reported;
  auto [last, added] = reported.try_emplace({handle, info->action, info->subactionPath}, result, rate);
  if (added || last->second != std::pair{result, rate}) {
    last->second = {result, rate};
    venture::xr::haptic_event("xr_pcm", handle, *info, 0, 0, 0, "xrGetDeviceSampleRateFB", result,
        {{"sample_rate", rate}, {"stage", "query"}});
  }
  return result;
}
}
