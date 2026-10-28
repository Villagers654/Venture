#pragma once
#include "scene_signature.h"
#include <openxr/openxr.h>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>
// XR_KHR_D3D11_enable for host-side OpenXR applications on Windows (the
// competitor bridges in the head-to-head use it). Frames, pacing and the frame
// log stay in runtime.cpp; this file owns only the D3D11 objects. Elsewhere
// every entry point reports the binding as absent.
namespace mock::d3d11 {
bool available();
bool active();
// Accepts an XrGraphicsBindingD3D11KHR chain head; false if `next` is not one.
bool bind(const void* next, XrResult* result);
void unbind();
XrResult requirements(XrInstance, XrSystemId, void* out);
std::vector<std::int64_t> formats();
XrResult create_images(const XrSwapchainCreateInfo&, int count, std::vector<void*>& images);
void destroy_images(std::vector<void*>& images);
XrResult enumerate_images(const std::vector<void*>& images, XrSwapchainImageBaseHeader* out);
// Signals frame `frame`'s completion fence behind all earlier device work.
void fence_frame(std::uint32_t frame);
// Fence value frame next-2 must reach, or 0 if it is already complete (state lock held).
std::uint64_t due(std::uint32_t next);
// Blocks without the state lock until the fence reaches `value`; returns the wait in ns.
std::int64_t wait(std::uint64_t value, std::uint64_t bound_ns);
void capture(void* image, const XrSwapchainCreateInfo&, const XrSwapchainSubImage&,
             const std::filesystem::path&);
// Repeat detection for bridges that resubmit an unchanged image: copies the three
// signature rows of `sub` (window) from frame `frame` into a staging ring and returns
// the hash and signature of the rows copied three frames earlier, if any
// (VENTURE_MOCK_CONTENT_HASH=1).
struct ContentSample {
  std::uint32_t frame = 0;
  std::uint64_t hash = 0;
  SceneSignature signature{};  // valid when frame != 0
  std::optional<SceneSignature> raw;  // 8-bit UNORM: the signature of the bytes as stored (not sRGB-encoded)
};
bool content_hash_enabled();
ContentSample sample_content(void* image, const XrSwapchainCreateInfo&, const XrSwapchainSubImage&,
                             const SignatureWindow& window, std::uint32_t frame);
}
