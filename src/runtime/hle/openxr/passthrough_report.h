#pragma once
#include "common/json.h"

namespace venture::hle::openxr {
// Keeps the latest passthrough backend selection (passthrough.cpp) in
// passthrough.json in the data directory, adding the time, for the launcher's
// Info dialog. Never throws.
void save_passthrough_report(Json report);
} // namespace venture::hle::openxr
