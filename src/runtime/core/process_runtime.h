#pragma once
#include "core/process.h"
#include <vector>
namespace venture {
void configure_game(const std::filesystem::path& install, std::string package_override = {});
void configure_game(const std::filesystem::path& install,
                    std::string package_override, std::string account_key);
void select_process_isa(GuestIsa isa);
void freeze_process_isa(); // Entry paths seal selection before any HLE registration.
std::vector<std::filesystem::path> extract_native_libraries();
std::filesystem::path resolve_guest_path(std::string_view path);
void write_inspection_report(const std::filesystem::path& output);
}
