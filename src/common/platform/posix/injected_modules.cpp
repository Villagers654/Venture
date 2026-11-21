// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/injected_modules.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace venture::injected_modules {
std::vector<Module> scan() {
  // Programs inject into Linux processes through LD_PRELOAD (separated by
  // spaces or colons, as ld.so reads it); Vulkan layers are filtered apart.
  std::vector<Module> out;
  const char *preload = std::getenv("LD_PRELOAD");
  std::string_view rest = preload ? preload : "";
  while (!rest.empty()) {
    const auto end = std::min(rest.find_first_of(": "), rest.size());
    if (end) out.push_back(Module{std::string(rest.substr(0, end))});
    rest.remove_prefix(std::min(end + 1, rest.size()));
  }
  return out;
}

std::vector<std::string> ancestor_images() {
  // /proc/<pid>/stat's fourth field is the parent; /proc/<pid>/exe its executable.
  std::vector<std::string> out;
  long pid = getppid();
  for (int depth = 0; depth < 8 && pid > 1; ++depth) {
    char image[4096];
    const auto link = "/proc/" + std::to_string(pid) + "/exe";
    const auto size = readlink(link.c_str(), image, sizeof image - 1);
    if (size <= 0) break;
    out.emplace_back(image, size_t(size));
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    std::getline(stat, line);
    // "pid (comm) state ppid ...": comm may hold spaces and parentheses.
    const auto close = line.rfind(')');
    if (close == std::string::npos) break;
    char state = 0;
    long parent = 0;
    if (std::sscanf(line.c_str() + close + 1, " %c %ld", &state, &parent) != 2) break;
    pid = parent;
  }
  return out;
}

std::vector<std::string> system_folders() { return {"/usr", "/bin", "/sbin", "/lib", "/lib64", "/app/bin"}; }

void set_process_variable(const char *name, const std::string &value) {
  if (setenv(name, value.c_str(), 1) != 0) throw std::runtime_error(std::string("Could not set ") + name);
}
}  // namespace venture::injected_modules
