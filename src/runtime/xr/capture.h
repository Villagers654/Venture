#pragma once
#include "xr/api.h"
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
namespace venture::xr {
// Debug readback only: the caller owns external synchronization of the XR queue.
bool capture_eye(const XrGraphicsBindingVulkanKHR&, PFN_vkGetInstanceProcAddr,
                 VkImage, VkFormat, const XrSwapchainSubImage&,
                 const std::filesystem::path&, std::string* error);
// Asynchronous capture: submit_eye records and submits the copy without
// waiting; write_pending_eye (any thread) waits for it and writes the PNG.
struct PendingCapture;
struct PendingCaptureDeleter { void operator()(PendingCapture*) const; };
using PendingCapturePtr = std::unique_ptr<PendingCapture, PendingCaptureDeleter>;
PendingCapturePtr submit_eye(const XrGraphicsBindingVulkanKHR&, PFN_vkGetInstanceProcAddr,
                             VkImage, VkFormat, const XrSwapchainSubImage&,
                             const std::filesystem::path&, std::string* error);
bool write_pending_eye(PendingCapture&, std::string* error);
// OpenXR treats non-sRGB formats (e.g. R8G8B8A8_UNORM) as linear values. encode_srgb_rgba8 sRGB-encodes 8-bit RGBA
// in place (alpha unchanged), as a compositor does for display; encode_srgb_on_finish makes finish_eye apply it.
void encode_srgb_rgba8(unsigned char* rgba, std::size_t bytes);
void encode_srgb_on_finish(PendingCapture&);
// Waits for a submitted capture and returns its RGBA8 pixels (rows of imageRect width).
bool finish_eye(PendingCapture&, std::vector<unsigned char>& pixels, std::string* error);
bool read_eye(const XrGraphicsBindingVulkanKHR&, PFN_vkGetInstanceProcAddr,
              VkImage, VkFormat, const XrSwapchainSubImage&, std::vector<unsigned char>&, std::string*);
void write_eye(const std::filesystem::path&, std::uint32_t, std::uint32_t,
               const std::vector<unsigned char>&, const XrRect2Di&);
}
