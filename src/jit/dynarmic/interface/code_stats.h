// Venture opt-in diagnostic: a stream of every block either backend emits,
// split at IR instruction boundaries, for offline host-instruction counts and
// the IR opcode mix (tools/jit_code_stats.py). Translated code is unchanged.
#pragma once
#include <string>
namespace Dynarmic::CodeStats {
// Opens the stream before blocks compile; false when the file cannot be created.
bool Enable(const std::string& path);
}  // namespace Dynarmic::CodeStats
