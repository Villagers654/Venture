#pragma once
#include <nlohmann/json_fwd.hpp>
namespace venture::hle::a32 {
// Actual A32 bindings, or a read-only projection of the registered A64 host
// signatures for packet validation before CPU/memory/loader integration.
nlohmann::json inspection_report();
// Install after A32 HLE registration and before loading inspected images.
void install_import_coverage_hook();
}
