#pragma once
#include "hle/vrapi/state.h"
#include "xr/sphere_images.h"
#include <array>
#include <deque>
#include <functional>
#include <span>
#include <vector>

namespace venture::hle::vrapi {
// VrApi layers the host cannot compose exactly (no cylinder, cube or equirect
// extension, as on SteamVR; elliptical, sheared or projective layer matrices;
// texture matrices; blend factors and color scales a host layer cannot
// express) are drawn into a projection layer at their place in the frame and
// at their true geometry (xr/sphere_images.h). Each run of consecutive ones in
// the same space shares one projection layer. Serialized by the VrApi state mutex.
struct DrawnEye {
  TextureSwapChain *chain = nullptr;
  int slot = 0;
  venture::xr::LayerPlacement placement{};
};
struct DrawnLayer {
  XrSpace space{};
  std::array<DrawnEye, 2> eyes{};
};
class DrawnLayers {
public:
  // Appends the run's projection placeholder to `layers` unless the layer
  // continues the run that placeholder ends.
  void add(const DrawnLayer &, std::vector<const XrCompositionLayerBaseHeader *> &layers);
  bool empty() const { return runs_.empty(); }
  // A ring slot's contents in a host image the caller holds acquired until
  // present() returned: memory row 0 first, all its layers.
  struct Source {
    VkImage image{};
    VkFormat format{};
    std::uint32_t layers = 1;
  };
  using Sources = std::function<XrResult(TextureSwapChain *, int slot, Source &)>;
  // Fetches every eye's source. A layer whose source the host cannot have
  // (FEATURE_UNSUPPORTED) is left out; other failures fail the frame.
  XrResult gather(const Sources &, std::vector<const XrCompositionLayerBaseHeader *> &layers);
  // After the sources' copies were submitted, before their release: draws
  // each run into its host chain and completes its placeholder. Also returns
  // chains no longer drawn into; nothing else runs without drawn layers.
  XrResult present(std::vector<const XrCompositionLayerBaseHeader *> &layers);

private:
  struct Run {
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    std::array<XrCompositionLayerProjectionView, 2> views{};
    std::vector<DrawnLayer> layers;
    std::vector<std::array<Source, 2>> sources;
  };
  std::deque<Run> runs_;
};
// Ring host chain images about to be destroyed.
void forget_drawn_sources(std::span<const VkImage>);
// Before the session goes: the host chains drawn into.
XrResult destroy_drawn_layers();
// With VENTURE_VULKAN_GPU_TIMING: GPU nanoseconds of the newest drawn frame read back.
std::uint64_t drawn_layers_gpu_ns();
} // namespace venture::hle::vrapi
