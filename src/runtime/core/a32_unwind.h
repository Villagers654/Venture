#pragma once
#include "core/guest.h"
#include <array>

namespace venture {
// Diagnostic EHABI virtual unwind only. No guest CPU/stack writes and no guest
// personality calls. Null means success; failure preserves the input registers.
const char* unwind_a32_frame(std::array<std::uint32_t, 16>& registers,
                             GuestAddr exidx, std::int32_t entries, bool return_address = false);
}
