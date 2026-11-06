#pragma once
#include "xr/api.h"
#include <array>
#include <cstdint>
#include <memory>
#include <span>

namespace venture::xr {
// One command buffer and one queue submission per frame for all drawn layers
// of a presenter, whichever chains (SphereImages) they go to: begin(), each
// chain's record(), submit(). Frames rotate through a few command buffers; a
// buffer is only waited for when the GPU is that many frames behind. Created
// on first use, so frames without drawn layers cost nothing. With
// VENTURE_VULKAN_GPU_TIMING the frame's GPU span is reported as "layer_draw".
class LayerFrame {
public:
  LayerFrame(const XrGraphicsBindingVulkanKHR &, PFN_vkGetInstanceProcAddr);
  ~LayerFrame();  // waits for its submissions
  // A command buffer in the recording state, or null on failure.
  VkCommandBuffer begin();
  XrResult submit();
  // Drops what begin() started recording (after a failed record()).
  void abandon();
  // Waits for every submission (before freeing what they use).
  void idle();
  // GPU nanoseconds of the newest frame whose timestamps were read (timing on).
  std::uint64_t last_gpu_ns() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
// Draws composition layers the host cannot compose (spheres, cylinders, cubes,
// VrApi tan-angle planes) into the eyes of a private host projection chain
// (shaders/layer.frag). Sources are either private copies of guest images that
// the caller fills (GuestImages) on the binding queue before the frame is
// submitted, or external images (VrApi's host chain images, acquired by the
// caller) in COLOR_ATTACHMENT_OPTIMAL, which record() samples and returns to
// that layout. Caller serializes this object and its LayerFrame.
class SphereImages {
public:
  // `target`: the host chain's create info (two array layers, one per eye).
  // `frame` submits what record() records and outlives this object.
  SphereImages(const XrGraphicsBindingVulkanKHR &, PFN_vkGetInstanceProcAddr, const XrSwapchainCreateInfo &target,
               std::span<const XrSwapchainImageVulkanKHR> images, LayerFrame &frame);
  ~SphereImages();
  // The private image for one source (`layers` array layers), created or
  // resized as needed; `fresh` until a copy into it was reported with
  // filled(). Unfilled sources are not drawn.
  XrResult source(std::uintptr_t key, VkFormat, VkExtent2D, VkImage &image, bool &fresh, std::uint32_t layers = 1);
  void filled(std::uintptr_t key);
  // An image owned elsewhere, all its `layers` sampled, in
  // COLOR_ATTACHMENT_OPTIMAL whenever render() is called with it.
  XrResult external(std::uintptr_t key, VkImage, VkFormat, std::uint32_t layers);
  // Frees the sources not named here.
  void retain(std::span<const std::uintptr_t> keys);
  // Frees the external sources of these images (before they are destroyed).
  void forget(std::span<const VkImage> images);
  // layer.frag push constants, in its order.
  using Constants = std::array<float, 32>;
  struct Draw {
    std::uintptr_t source;
    std::uint32_t eye;
    Constants constants;
    VkRect2D area{};  // the pixels it can cover (layer_bounds); empty: none
  };
  // Records into `command` (from frame.begin()): clears both eyes of host
  // image `index`, then draws in order; the target ends in
  // COLOR_ATTACHMENT_OPTIMAL, released after frame.submit().
  XrResult record(VkCommandBuffer command, std::uint32_t index, std::span<const Draw>);
  const XrSwapchainCreateInfo &target() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// How layer.frag finds a pixel's place on a layer.
enum class LayerShape : std::uint32_t {
  // VrApi's tan-angle matrix: (S, T, Q) from the ray in the layer frame (x, y, -1).
  plane = 0,
  // The unit cylinder about +Y; (a, b) = (angle from -Z towards +X, height).
  cylinder = 1,
  // The unit sphere around the origin; (a, b) = (longitude as above, latitude).
  sphere = 2,
  // A sphere at infinity: directions only.
  far_sphere = 3,
  // Cube directions, faces in Vulkan order from the source layer up.
  cube = 4,
};
// One layer for one eye. `frame` is the layer's frame in the view's space and
// `local` (row-major, column vectors) takes frame coordinates to shape space;
// for a plane it is the VrApi tan-angle matrix, whose texture matrix is `map`.
// `map` takes (a, b) (a plane: (S/Q, T/Q)) to normalized source coordinates,
// and only places inside `clip` (min x, min y, max x, max y) show. A cube has
// neither. Factors: 0 zero, 1 one, 2 source alpha, 5 one minus source alpha.
struct LayerPlacement {
  LayerShape shape;
  XrPosef frame;
  float local[4][4];
  float map[2][3];
  float clip[4];
  std::uint32_t source_layer;
  std::uint32_t source_factor, destination_factor;
  XrColor4f scale{1, 1, 1, 1}, bias{0, 0, 0, 0};  // applied to each texel, before blending
};
// The push constants for drawing `layer` into a view of `extent` pixels.
SphereImages::Constants layer_constants(const LayerPlacement &layer, const XrView &view, XrExtent2Di extent);
// A rectangle of that view holding every pixel the layer covers (the whole
// view where that cannot be bounded cheaply), for the draw's scissor.
VkRect2D layer_bounds(const LayerPlacement &layer, const XrView &view, XrExtent2Di extent);
} // namespace venture::xr
