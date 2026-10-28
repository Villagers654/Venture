// SDL3 audio as a miniaudio custom backend.
//
// Each miniaudio device direction is one SDL audio stream bound to SDL's
// default playback or recording device (or an enumerated one). SDL picks the
// host audio stack (PipeWire, PulseAudio, ALSA, in that order) and moves
// default-device streams when the system default changes. SDL converts
// between the stream format, which is what miniaudio asks for, and the
// device's. SDL calls back on its own audio thread; the callbacks use
// buffers sized at device creation and never allocate.
#include "platform/audio_backend.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace venture::platform {
namespace {
struct Direction {
  ma_device* device = nullptr;
  SDL_AudioStream* stream = nullptr;
  std::uint32_t channels = 0;
  std::vector<float> scratch;  // whole frames
};
struct Device {
  Direction playback, capture;
};
std::mutex devices_mutex;
std::map<ma_device*, std::unique_ptr<Device>> devices;

SDL_AudioDeviceID sdl_id(ma_device_type type, const ma_device_id* id) {
  if (id) return SDL_AudioDeviceID(id->custom.i);
  return type == ma_device_type_playback ? SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK : SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
}
bool is_default(SDL_AudioDeviceID id) {
  return id == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK || id == SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
}
void fill_info(SDL_AudioDeviceID id, ma_device_info& info) {
  info.id.custom.i = int(id);
  // A default ID names the device it currently resolves to.
  const char* name = SDL_GetAudioDeviceName(id);
  std::snprintf(info.name, sizeof info.name, "%s", name ? name : "");
  info.isDefault = is_default(id) ? MA_TRUE : MA_FALSE;
  SDL_AudioSpec spec{};
  int frames = 0;
  if (SDL_GetAudioDeviceFormat(id, &spec, &frames)) {
    // SDL converts any stream format; report what the device runs at.
    auto& format = info.nativeDataFormats[info.nativeDataFormatCount++];
    format = {ma_format_f32, ma_uint32(spec.channels), ma_uint32(spec.freq), 0};
  }
}

void SDLCALL play(void* user, SDL_AudioStream* stream, int additional, int) {
  auto& d = *static_cast<Direction*>(user);
  const auto frame_bytes = int(d.channels * sizeof(float));
  auto frames = ma_uint32(additional / frame_bytes + (additional % frame_bytes != 0));
  const auto chunk = ma_uint32(d.scratch.size() / d.channels);
  while (frames) {
    const auto n = std::min(frames, chunk);
    ma_device_handle_backend_data_callback(d.device, d.scratch.data(), nullptr, n);
    SDL_PutAudioStreamData(stream, d.scratch.data(), int(n) * frame_bytes);
    frames -= n;
  }
}
void SDLCALL record(void* user, SDL_AudioStream* stream, int, int) {
  auto& d = *static_cast<Direction*>(user);
  const auto frame_bytes = int(d.channels * sizeof(float));
  const auto capacity = int(d.scratch.size() * sizeof(float));
  // Whole frames only; a partial frame stays in the stream for next time.
  while (SDL_GetAudioStreamAvailable(stream) >= frame_bytes) {
    const auto want = std::min(capacity, SDL_GetAudioStreamAvailable(stream) / frame_bytes * frame_bytes);
    const auto got = SDL_GetAudioStreamData(stream, d.scratch.data(), want);
    if (got <= 0) break;
    ma_device_handle_backend_data_callback(d.device, nullptr, d.scratch.data(), ma_uint32(got / frame_bytes));
  }
}

ma_result context_uninit(ma_context*) {
  SDL_QuitSubSystem(SDL_INIT_AUDIO);
  return MA_SUCCESS;
}
ma_result enumerate(ma_context* context, ma_enum_devices_callback_proc callback, void* user) {
  for (auto type : {ma_device_type_playback, ma_device_type_capture}) {
    ma_device_info info{};
    fill_info(sdl_id(type, nullptr), info);
    if (!callback(context, type, &info, user)) return MA_SUCCESS;
    int count = 0;
    SDL_AudioDeviceID* ids =
        type == ma_device_type_playback ? SDL_GetAudioPlaybackDevices(&count) : SDL_GetAudioRecordingDevices(&count);
    bool more = true;
    for (int i = 0; ids && more && i < count; ++i) {
      info = {};
      fill_info(ids[i], info);
      more = callback(context, type, &info, user);
    }
    SDL_free(ids);
    if (!more) return MA_SUCCESS;
  }
  return MA_SUCCESS;
}
ma_result device_info(ma_context*, ma_device_type type, const ma_device_id* id, ma_device_info* info) {
  fill_info(sdl_id(type, id), *info);
  return MA_SUCCESS;
}
ma_result open(ma_device* device, ma_device_type type, const ma_device_config& config,
               ma_device_descriptor& descriptor, Direction& d) {
  if (descriptor.shareMode == ma_share_mode_exclusive) return MA_SHARE_MODE_NOT_SUPPORTED;
  const auto channels = descriptor.channels ? descriptor.channels : 2u;
  const auto rate = descriptor.sampleRate ? descriptor.sampleRate : 48000u;
  descriptor.format = ma_format_f32;
  descriptor.channels = channels;
  descriptor.sampleRate = rate;
  ma_channel_map_init_standard(ma_standard_channel_map_default, descriptor.channelMap, MA_MAX_CHANNELS, channels);
  descriptor.periodSizeInFrames =
      ma_calculate_buffer_size_in_frames_from_descriptor(&descriptor, rate, config.performanceProfile);
  if (!descriptor.periodCount) descriptor.periodCount = 2;
  d.device = device;
  d.channels = channels;
  // Callbacks may ask for more than a period; larger requests are served in
  // period-sized pieces.
  d.scratch.assign(std::size_t(descriptor.periodSizeInFrames) * channels, 0.0f);
  // The device's own buffer follows the requested period, as with miniaudio's
  // built-in backends; SDL reads the hint when it opens the device.
  SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, std::to_string(descriptor.periodSizeInFrames).c_str());
  const SDL_AudioSpec spec{SDL_AUDIO_F32, int(channels), int(rate)};
  d.stream = SDL_OpenAudioDeviceStream(sdl_id(type, descriptor.pDeviceID), &spec,
                                       type == ma_device_type_playback ? play : record, &d);
  return d.stream ? MA_SUCCESS : MA_FAILED_TO_OPEN_BACKEND_DEVICE;
}
ma_result device_uninit(ma_device* device) {
  std::unique_ptr<Device> owned;
  {
    std::lock_guard lock(devices_mutex);
    auto it = devices.find(device);
    if (it == devices.end()) return MA_SUCCESS;
    owned = std::move(it->second);
    devices.erase(it);
  }
  // Destroying a bound stream waits for a callback in progress.
  for (auto* d : {&owned->playback, &owned->capture})
    if (d->stream) SDL_DestroyAudioStream(d->stream);
  return MA_SUCCESS;
}
ma_result device_init(ma_device* device, const ma_device_config* config, ma_device_descriptor* playback,
                      ma_device_descriptor* capture) {
  if (config->deviceType == ma_device_type_loopback) return MA_DEVICE_TYPE_NOT_SUPPORTED;
  auto owned = std::make_unique<Device>();
  auto& d = *owned;
  {
    std::lock_guard lock(devices_mutex);
    devices[device] = std::move(owned);
  }
  ma_result result = MA_SUCCESS;
  if (config->deviceType == ma_device_type_capture || config->deviceType == ma_device_type_duplex)
    result = open(device, ma_device_type_capture, *config, *capture, d.capture);
  if (result == MA_SUCCESS &&
      (config->deviceType == ma_device_type_playback || config->deviceType == ma_device_type_duplex))
    result = open(device, ma_device_type_playback, *config, *playback, d.playback);
  if (result != MA_SUCCESS) device_uninit(device);
  return result;
}
Device* find(ma_device* device) {
  std::lock_guard lock(devices_mutex);
  auto it = devices.find(device);
  return it == devices.end() ? nullptr : it->second.get();
}
ma_result device_start(ma_device* device) {
  auto* d = find(device);
  if (!d) return MA_INVALID_OPERATION;
  for (auto* direction : {&d->capture, &d->playback})
    if (direction->stream && !SDL_ResumeAudioStreamDevice(direction->stream)) return MA_FAILED_TO_START_BACKEND_DEVICE;
  return MA_SUCCESS;
}
ma_result device_stop(ma_device* device) {
  auto* d = find(device);
  if (!d) return MA_INVALID_OPERATION;
  for (auto* direction : {&d->playback, &d->capture})
    if (direction->stream && !SDL_PauseAudioStreamDevice(direction->stream)) return MA_FAILED_TO_STOP_BACKEND_DEVICE;
  return MA_SUCCESS;
}
ma_result context_init(ma_context*, const ma_context_config*, ma_backend_callbacks* callbacks) {
  // The name desktop mixers show for the stream.
  if (!SDL_GetHint(SDL_HINT_APP_NAME)) SDL_SetHint(SDL_HINT_APP_NAME, "Venture");
  // Reference counted by SDL: each context holds the subsystem.
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return MA_NO_BACKEND;
  callbacks->onContextInit = context_init;
  callbacks->onContextUninit = context_uninit;
  callbacks->onContextEnumerateDevices = enumerate;
  callbacks->onContextGetDeviceInfo = device_info;
  callbacks->onDeviceInit = device_init;
  callbacks->onDeviceUninit = device_uninit;
  callbacks->onDeviceStart = device_start;
  callbacks->onDeviceStop = device_stop;
  return MA_SUCCESS;
}
}  // namespace

void configure_audio_context(ma_context_config& config) { config.custom.onContextInit = context_init; }
}  // namespace venture::platform
