#pragma once
namespace venture {
// Called once by the runtime before resolving ELF imports.
void register_all_hle_libraries();
}  // namespace venture
