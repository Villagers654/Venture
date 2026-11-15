#pragma once
// Registered implicit OpenXR API layers, read the way the Khronos loader reads
// them (src/loader/manifest_file.cpp of the pinned SDK), for the host instance
// and for debug reports.
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace venture::openxr_layers {
struct Manifest {
  std::filesystem::path manifest;
  std::string name;
  std::string library_path;       // as written in the manifest
  std::filesystem::path library;  // absolute, or relative to the manifest resolved; a bare name stays bare
  std::string disable_environment, enable_environment;
  std::map<std::string, std::string> functions;  // the manifest's renamed entry points
  // Why the loader cannot use the layer's files; empty when they look usable.
  // The loader passes over a missing or invalid manifest and a missing library,
  // but a library it cannot load fails xrCreateInstance when no other layer loads.
  std::string problem;
};
// The implicit manifests the loader finds, in its order: Windows HKLM then
// HKCU SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit values whose data is 0;
// elsewhere openxr/1/api_layers/implicit.d in the XDG config and data dirs.
// Registered files that do not exist are listed too (the loader skips them).
std::vector<std::filesystem::path> implicit_manifests();
// The manifest at `path` with its files checked.
Manifest read(const std::filesystem::path &path);
// The manifest from its text; `problem` covers the text only.
Manifest parse(const std::filesystem::path &path, std::string_view text);
// Why `library` cannot be loaded into this process: missing, unreadable, or
// not a library for this architecture; empty when it looks loadable.
std::string library_problem(const std::filesystem::path &library);
// Whether the loader enables the layer in this environment (disable_environment
// unset; enable_environment, when the manifest has one, set).
bool enabled(const Manifest &layer);
// Whether `name` is set in the process environment the loader reads.
bool environment_set(const char *name);
}  // namespace venture::openxr_layers
