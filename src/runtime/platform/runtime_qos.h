#pragma once

namespace venture::platform {
// Apply once in the game worker, after logging is configured and before guest
// threads start. Windows schedules the cores; this does not set affinity.
void initialize_runtime_qos();
}
