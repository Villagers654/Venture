#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>
#include <string>
namespace venture::java {
struct AppLaunchOptions {
  std::filesystem::path java_home;
  std::filesystem::path runtime_directory;
  std::vector<std::string> class_path;
  std::uint32_t parent_pid = 0;
  double run_seconds = 0; // zero: application/launcher controls the lifetime
  bool defer_gpu = false; // diagnostic: initialize on the first guest EGL/Vulkan call
};
// ProcessConfig, native extraction and the HLE registry must be ready.
void launch_android_app(const AppLaunchOptions&);
}
