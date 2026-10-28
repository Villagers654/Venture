#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include "core/guest.h"

namespace venture::android {
class Looper;
struct InputQueue;
struct PointerCoords {
  std::int32_t id = 0, tool_type = 1;
  // AOSP input.h axis numbers: X=0, Y=1, PRESSURE=2, SIZE=3, ...
  float axes[64]{};
};
struct InputEvent {
  std::int32_t type = 1, device_id = 0, source = 0, action = 0, flags = 0;
  std::int32_t key_code = 0, scan_code = 0, meta_state = 0, repeat_count = 0;
  std::int32_t button_state = 0, edge_flags = 0;
  std::int64_t down_time = 0, event_time = 0;
  float x_offset = 0, y_offset = 0, x_precision = 1, y_precision = 1;
  std::vector<PointerCoords> pointers;
};
InputQueue* create_input_queue();
void destroy_input_queue(InputQueue*);
// Framework supplies monotonic nanosecond times. Completion runs on the
// thread calling finishEvent; marshal it to the UI thread if necessary.
bool enqueue_input(InputQueue*, InputEvent event, std::function<void(bool handled)> completion = {});
void attach_input_queue(InputQueue*, Looper*, int ident, GuestAddr callback, GuestAddr data);
void detach_input_queue(InputQueue*);
int input_has_events(InputQueue*);
int input_get_event(InputQueue*, GuestAddr* event);
void input_finish_event(InputQueue*, GuestAddr event, bool handled);
std::shared_ptr<const InputEvent> pin_input_event(GuestAddr);
}  // namespace venture::android
