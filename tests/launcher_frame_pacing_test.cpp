#include "launcher/frame_pacing.h"
#include <doctest/doctest.h>

using venture::launcher::FramePacing;

TEST_CASE("Launcher enters idle pacing without requiring a focused widget to deactivate") {
  FramePacing pacing(0);
  CHECK(pacing.ready(0, false));
  pacing.presented(0);
  CHECK(pacing.wait(1, false) == 16);
  pacing.presented(1300);
  CHECK_FALSE(pacing.ready(1799, false));
  CHECK(pacing.ready(1800, false));
  CHECK(pacing.wait(1301, false) == 250); // background work is polled sooner
}
TEST_CASE("Launcher input and worker completions wake idle rendering without exceeding the frame limit") {
  FramePacing pacing(0);
  pacing.presented(1300);
  pacing.wake(1350);
  CHECK(pacing.ready(1350, false));
  pacing.presented(1350);
  for (unsigned time = 1351; time < 1367; ++time) {
    pacing.wake(time);
    CHECK_FALSE(pacing.ready(time, false));
  }
  CHECK(pacing.ready(1367, false));
  pacing.presented(1367);
  CHECK(pacing.wait(1367, false) == 17);
}
TEST_CASE("Launcher hidden windows keep bounded polling and resume immediately on restore") {
  FramePacing pacing(0);
  pacing.presented(0);
  for (unsigned time = 250; time < 10000; time += 250) {
    pacing.wake(time);
    CHECK_FALSE(pacing.ready(time, true));
    CHECK(pacing.wait(time, true) == 250);
  }
  pacing.wake(10000);
  CHECK(pacing.ready(10000, false));
}
