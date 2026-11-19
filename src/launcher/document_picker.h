// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The system's document picker where the host hands files out by address
// (Android's Storage Access Framework): the picks reach `done` as addresses
// that common/library/import.h's documents read. False where the host has
// none; the launcher then uses SDL's file dialogs, which give paths.
#include <functional>
#include <string>
#include <vector>

namespace venture::launcher {
// `done` runs on another thread, with nothing when the user cancelled.
bool pick_documents(bool folder, std::function<void(std::vector<std::string>)> done);
}  // namespace venture::launcher
