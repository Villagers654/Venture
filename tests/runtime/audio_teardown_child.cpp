#include "audio/audio.h"
#include "core/av_telemetry.h"

int main() {
  // Match a game that opens audio before the first diagnostic event. No device
  // is opened: only the backend's static lifetime and final snapshot matter.
  (void)venture::audio::Backend::get();
  venture::av::emit({{"kind", "before_static_teardown"}});
  return 0;
}
