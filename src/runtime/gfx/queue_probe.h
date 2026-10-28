#pragma once

namespace venture::gfx {
// With shutdown snapshots enabled, each snapshot reports whether the shared
// graphics queue still completes work. Inert otherwise.
void register_queue_probe();
}
