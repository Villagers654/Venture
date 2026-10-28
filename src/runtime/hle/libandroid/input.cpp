#include "android/errors.h"
#include "hle/libandroid/parts.h"
#include "android/input.h"
#include "android/looper.h"
namespace venture::hle::android_hle {
namespace {
using namespace venture::android;
template <auto Member>
auto field(GuestAddr p) -> std::remove_cvref_t<decltype(InputEvent{}.*Member)> {
  auto e = pin_input_event(p);
  return e ? e.get()->*Member : std::remove_cvref_t<decltype(InputEvent{}.*Member)>{};
}
std::uint64_t count(GuestAddr p) {
  auto e = pin_input_event(p);
  return e ? e->pointers.size() : 0;
}
std::int32_t pointer_id(GuestAddr p, std::uint64_t i) {
  auto e = pin_input_event(p);
  return e && i < e->pointers.size() ? e->pointers[i].id : -1;
}
std::int32_t tool(GuestAddr p, std::uint64_t i) {
  auto e = pin_input_event(p);
  return e && i < e->pointers.size() ? e->pointers[i].tool_type : 0;
}
float axis(GuestAddr p, std::int32_t a, std::uint64_t i) {
  auto e = pin_input_event(p);
  return e && a >= 0 && a < 64 && i < e->pointers.size() ? e->pointers[i].axes[a] : 0;
}
template <int Axis>
float coordinate(GuestAddr p, std::uint64_t i) {
  return axis(p, Axis, i);
}
template <int Axis>
float raw_coordinate(GuestAddr p, std::uint64_t i) {
  auto e = pin_input_event(p);
  return axis(p, Axis, i) - (e ? (Axis == 0 ? e->x_offset : e->y_offset) : 0);
}
std::uint64_t history_size(GuestAddr) { return 0; }    // Framework emits individual samples, never batches.
int predisptach(InputQueue*, GuestAddr) { return 0; }  // No IME intercepts native key events.
void finish(InputQueue* q, GuestAddr e, std::int32_t handled) { input_finish_event(q, e, handled != 0); }
}  // namespace
void register_input(HleLibrary& lib) {
  add<&attach_input_queue>(lib, "AInputQueue_attachLooper");
  add<&detach_input_queue>(lib, "AInputQueue_detachLooper");
  add<&input_has_events>(lib, "AInputQueue_hasEvents");
  add<&input_get_event>(lib, "AInputQueue_getEvent");
  add<&finish>(lib, "AInputQueue_finishEvent");
  add<&predisptach>(lib, "AInputQueue_preDispatchEvent");
  add<&field<&InputEvent::type>>(lib, "AInputEvent_getType");
  add<&field<&InputEvent::device_id>>(lib, "AInputEvent_getDeviceId");
  add<&field<&InputEvent::source>>(lib, "AInputEvent_getSource");
  add<&field<&InputEvent::action>>(lib, "AKeyEvent_getAction");
  add<&field<&InputEvent::flags>>(lib, "AKeyEvent_getFlags");
  add<&field<&InputEvent::key_code>>(lib, "AKeyEvent_getKeyCode");
  add<&field<&InputEvent::scan_code>>(lib, "AKeyEvent_getScanCode");
  add<&field<&InputEvent::meta_state>>(lib, "AKeyEvent_getMetaState");
  add<&field<&InputEvent::repeat_count>>(lib, "AKeyEvent_getRepeatCount");
  add<&field<&InputEvent::down_time>>(lib, "AKeyEvent_getDownTime");
  add<&field<&InputEvent::event_time>>(lib, "AKeyEvent_getEventTime");
  add<&field<&InputEvent::action>>(lib, "AMotionEvent_getAction");
  add<&field<&InputEvent::flags>>(lib, "AMotionEvent_getFlags");
  add<&field<&InputEvent::meta_state>>(lib, "AMotionEvent_getMetaState");
  add<&field<&InputEvent::button_state>>(lib, "AMotionEvent_getButtonState");
  add<&field<&InputEvent::edge_flags>>(lib, "AMotionEvent_getEdgeFlags");
  add<&field<&InputEvent::down_time>>(lib, "AMotionEvent_getDownTime");
  add<&field<&InputEvent::event_time>>(lib, "AMotionEvent_getEventTime");
  add<&field<&InputEvent::x_offset>>(lib, "AMotionEvent_getXOffset");
  add<&field<&InputEvent::y_offset>>(lib, "AMotionEvent_getYOffset");
  add<&field<&InputEvent::x_precision>>(lib, "AMotionEvent_getXPrecision");
  add<&field<&InputEvent::y_precision>>(lib, "AMotionEvent_getYPrecision");
  add<&coordinate<0>>(lib, "AMotionEvent_getX");
  add<&coordinate<1>>(lib, "AMotionEvent_getY");
  add<&coordinate<2>>(lib, "AMotionEvent_getPressure");
  add<&coordinate<3>>(lib, "AMotionEvent_getSize");
  add<&coordinate<4>>(lib, "AMotionEvent_getTouchMajor");
  add<&coordinate<5>>(lib, "AMotionEvent_getTouchMinor");
  add<&coordinate<6>>(lib, "AMotionEvent_getToolMajor");
  add<&coordinate<7>>(lib, "AMotionEvent_getToolMinor");
  add<&coordinate<8>>(lib, "AMotionEvent_getOrientation");
  add<&raw_coordinate<0>>(lib, "AMotionEvent_getRawX");
  add<&raw_coordinate<1>>(lib, "AMotionEvent_getRawY");
  add<&axis>(lib, "AMotionEvent_getAxisValue");
  add<&count>(lib, "AMotionEvent_getPointerCount");
  add<&pointer_id>(lib, "AMotionEvent_getPointerId");
  add<&tool>(lib, "AMotionEvent_getToolType");
  add<&history_size>(lib, "AMotionEvent_getHistorySize");
}
}  // namespace venture::hle::android_hle
