#include "platform/runtime_qos.h"
#include "common/log.h"
#include <windows.h>
#include <mutex>

namespace venture::platform {
void initialize_runtime_qos() {
  static std::once_flag once;
  std::call_once(once, [] {
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED |
                        PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    state.StateMask = 0;
    const bool success = SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling,
                                               &state, sizeof(state)) != FALSE;
    const auto error = success ? 0 : GetLastError();
    // The worker's hidden Vulkan window does not express its headset-facing
    // foreground workload. Explicit High QoS also keeps timer requests honored.
    log::write(success ? log::Level::info : log::Level::warn, "qos",
        std::format("Runtime QoS request: high=true honor_timer_resolution=true success={} error={}", success, error));
  });
}
}
