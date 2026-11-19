#pragma once
#include <string_view>

namespace venture::hle::libc {
// Runs a command line as Android's sh -c would, for the simple file commands
// apps and mods run with system() (shell.cpp). Returns the shell's exit
// status: 127 for a command line it cannot run.
int run_shell_command(std::string_view line);
}  // namespace venture::hle::libc
