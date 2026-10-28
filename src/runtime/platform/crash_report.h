#pragma once
#include <string_view>

namespace venture::platform {
// Install before guest/JIT execution, preserving guarded guest-memory recovery.
// POSIX handlers installed later chain back here for host faults only.
void install_crash_report(std::string_view log_path, std::string_view package);
// POSIX alternate signal stacks are per thread; call at host thread attachment.
void prepare_crash_stack();
}
