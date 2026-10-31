#include "platform/java_host.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <strings.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <system_error>
extern char** environ;

namespace venture::java::platform {
// Venture owns SIGINT/SIGTERM (RuntimeStop) and SIGSEGV/SIGBUS ordering. -Xrs
// keeps HotSpot from turning a stop request into System.exit; its own crash
// and implicit-null-check handlers are unaffected.
std::vector<std::string> jvm_host_options() { return {"-Xrs"}; }
void* loaded_agent_symbol(const std::filesystem::path& agent, const char* name) {
  auto module = dlopen(agent.c_str(), RTLD_NOW | RTLD_NOLOAD);
  if (!module) throw std::runtime_error("Bundled JVM agent is not loaded");
  auto symbol = dlsym(module, name);
  // JNI_CreateJavaVM retains the module; balance only this lookup's reference.
  dlclose(module);
  if (!symbol) throw std::runtime_error(std::string("Missing JVM agent export: ") + name);
  return symbol;
}
void* jvm_entry(const std::filesystem::path& home) {
  auto module = dlopen((home / "lib/server/libjvm.so").c_str(), RTLD_NOW | RTLD_GLOBAL);
  if (!module) throw std::runtime_error(dlerror());
  auto entry = dlsym(module, "JNI_CreateJavaVM");
  if (!entry) throw std::runtime_error(dlerror());
  return entry;
}
void* executable_stub(std::span<const std::byte> bytes) {
  void* memory = mmap(nullptr, bytes.size(), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (memory == MAP_FAILED) throw std::system_error(errno, std::generic_category(), "mmap(JNI stub)");
  std::memcpy(memory, bytes.data(), bytes.size());
  if (mprotect(memory, bytes.size(), PROT_READ | PROT_EXEC)) {
    int error = errno;
    munmap(memory, bytes.size());
    throw std::system_error(error, std::generic_category(), "mprotect(JNI stub)");
  }
  __builtin___clear_cache(static_cast<char*>(memory), static_cast<char*>(memory) + bytes.size());
  return memory;
}
int run_process(const std::vector<std::string>& args, const std::vector<std::string>& unset_environment,
                const std::filesystem::path* output) {
  if (args.empty()) throw std::invalid_argument("empty process arguments");
  std::vector<char*> argv;
  for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
  argv.push_back(nullptr);
  std::vector<char*> envp;
  for (char** entry = environ; *entry; ++entry) {
    const std::string_view text(*entry);
    const auto name = text.substr(0, text.find('='));
    const bool removed = std::any_of(unset_environment.begin(), unset_environment.end(), [&](const std::string& unset) {
      return unset.size() == name.size() && strncasecmp(unset.data(), name.data(), name.size()) == 0;
    });
    if (!removed) envp.push_back(*entry);
  }
  envp.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  if (output) {
    posix_spawn_file_actions_addopen(&actions, 1, output->c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    posix_spawn_file_actions_adddup2(&actions, 1, 2);
  }
  pid_t pid;
  int error = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), envp.data());
  posix_spawn_file_actions_destroy(&actions);
  if (error) throw std::system_error(error, std::generic_category(), "posix_spawn(converter)");
  int status;
  while (waitpid(pid, &status, 0) < 0)
    if (errno != EINTR) throw std::system_error(errno, std::generic_category(), "waitpid(converter)");
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
}  // namespace venture::java::platform
