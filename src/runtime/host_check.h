// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// venture-runtime --host-check (docs/design/native-headsets.md, section 9):
// what this runtime finds on the host it runs on, as plain lines for the
// launcher's headset check and the debug report. It runs where Play runs (in
// the Flatpak, inside its sandbox): the host CPU facts, the CPU engine for A64
// and A32 games and why, the GLES path, and the OpenXR runtime the bundled
// loader reaches, with its version, extensions and headset.
#include <string>

namespace venture {
std::string host_check();
}  // namespace venture
