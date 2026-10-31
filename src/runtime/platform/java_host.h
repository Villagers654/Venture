#pragma once
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace venture::java::platform {
// Module stays loaded for process lifetime: HotSpot cannot safely be unloaded
// and recreated, even after DestroyJavaVM.
void* jvm_entry(const std::filesystem::path& home);
// Resolve only an agent already loaded by this JVM from the specified bundle.
void* loaded_agent_symbol(const std::filesystem::path& agent, const char* name);
void* executable_stub(std::span<const std::byte> bytes);  // RW then RX, process lifetime
// No shell interpretation. The child inherits this process's environment except
// the variables named in unset_environment (compared case-insensitively). With
// output set, the child's stdout and stderr are written to that file.
int run_process(const std::vector<std::string>& args, const std::vector<std::string>& unset_environment = {},
                const std::filesystem::path* output = nullptr);
// HotSpot options this host requires; appended before caller-supplied options.
std::vector<std::string> jvm_host_options();
inline constexpr char class_path_separator =
#ifdef _WIN32
    ';';
#else
    ':';
#endif
}  // namespace venture::java::platform
