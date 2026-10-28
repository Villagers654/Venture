#include "android/errors.h"
#include "hle/libandroid/parts.h"
#include "android/native_window.h"
#include "android/framework_bridge.h"
#include <cmath>
namespace venture::hle::android_hle {
namespace {
using namespace venture::android;
NativeWindow* from_surface(void* env, _jobject* surface) {
  if (!surface) return nullptr;
  auto bridge = surface_bridge();
  if (bridge.from_surface) return bridge.from_surface(env, surface);
  log::warn("window", "ANativeWindow_fromSurface: framework surface bridge is not installed");
  fail(err::unsupported);
  return nullptr;
}
_jobject* to_surface(void* env, NativeWindow* window) {
  auto win = pin_native_window(h2g(window));
  if (!win) return nullptr;
  auto bridge = surface_bridge();
  if (bridge.to_surface) return bridge.to_surface(env, win.get());
  log::warn("window", "ANativeWindow_toSurface: framework surface bridge is not installed");
  return nullptr;
}
template <int Field>
int dimension(NativeWindow* p) {
  auto win = pin_native_window(h2g(p));
  if (!win) return -err::invalid;
  std::lock_guard lock(win->lock);
  return Field == 0 ? win->width : Field == 1 ? win->height : win->format;
}
int frame_rate(NativeWindow* p, float rate, std::int8_t compatibility) {
  auto win = pin_native_window(h2g(p));
  if (!win || !std::isfinite(rate) || rate < 0 || compatibility < 0 || compatibility > 1)
    return -err::invalid;
  std::lock_guard lock(win->lock);
  win->requested_frame_rate = rate;
  // Android treats this as a content-rate hint, not a promise to switch display modes.
  return 0;
}
}  // namespace
void register_windows(HleLibrary& lib) {
  add<&from_surface>(lib, "ANativeWindow_fromSurface");
  add<&to_surface>(lib, "ANativeWindow_toSurface");
  add<&android::acquire>(lib, "ANativeWindow_acquire");
  add<&android::release>(lib, "ANativeWindow_release");
  add<&dimension<0>>(lib, "ANativeWindow_getWidth");
  add<&dimension<1>>(lib, "ANativeWindow_getHeight");
  add<&dimension<2>>(lib, "ANativeWindow_getFormat");
  add<&frame_rate>(lib, "ANativeWindow_setFrameRate");
  add<&android::set_window_geometry>(lib, "ANativeWindow_setBuffersGeometry");
  add<&android::lock_window>(lib, "ANativeWindow_lock");
  add<&android::unlock_window>(lib, "ANativeWindow_unlockAndPost");
}
}  // namespace venture::hle::android_hle
