#pragma once
#include <filesystem>
#include <span>

namespace venture {
struct ProcessConfig;
namespace engine_policy {
struct RendererCapabilities {
  bool variable_rate_shading = false;
};
// Apply supported engine configuration before guest initialization. Only an
// owned configuration block is replaced; original application payloads stay intact.
void prepare_renderer_defaults(const ProcessConfig& config,
    std::span<const std::filesystem::path> libraries, RendererCapabilities capabilities) noexcept;
}
}
