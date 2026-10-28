#pragma once
#include <miniaudio.h>

namespace venture::platform {
// Selects the host audio stack for a miniaudio context. Windows keeps
// miniaudio's own backends (WASAPI). Elsewhere SDL3 audio serves as a custom
// backend: PipeWire, then PulseAudio, then ALSA, in SDL's order, with its
// default devices following the system default.
void configure_audio_context(ma_context_config& config);
} // namespace venture::platform
