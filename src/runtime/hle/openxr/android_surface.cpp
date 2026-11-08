#include "xr/dev_dump.h"
#include "android/framework_bridge.h"
#include "android/native_window.h"
#include "common/log.h"
#include "core/process.h"
#include "gfx/gpu_device.h"
#include "hle/openxr/image_upload.h"
#include "hle/openxr/state.h"
#include "java/jvm.h"
#include "xr/queue.h"
#include <jni.h>
#include <cstring>

namespace venture::hle::openxr {
// The window owns completed producer buffers; the XR consumer publishes those bytes
// into real runtime-owned images on the session's Vulkan device. No guest
// acquire/release operations are permitted on an Android Surface swapchain.
// https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrCreateSwapchainAndroidSurfaceKHR.html
struct AndroidSurface {
  android::NativeWindow* window = nullptr;
  jobject surface = nullptr;
  bool global_surface = false;
  ImageUpload upload;
  bool initialized = false;
  std::uint64_t published = 0;
  std::vector<std::size_t> source_columns;
  std::uint32_t sampled_width = 0;
  XrColorSpaceFB published_color = XR_COLOR_SPACE_REC709_FB;

  ~AndroidSurface() {
    if (surface && java::host_vm()) {
      auto* env = java::host_env();
      // Cleanup may run after a failed JNI allocation. Preserve that failure
      // while releasing the Surface's separately acquired native reference.
      auto pending = env->ExceptionOccurred();
      if (pending) env->ExceptionClear();
      auto cls = env->GetObjectClass(surface);
      auto release = cls ? env->GetMethodID(cls, "release", "()V") : nullptr;
      if (release) {
        env->CallVoidMethod(surface, release);
        env->ExceptionCheck();
      }
      if (cls) env->DeleteLocalRef(cls);
      if (global_surface) env->DeleteGlobalRef(surface);
      else env->DeleteLocalRef(surface);
      if (pending) {
        if (env->ExceptionCheck()) {
          log::warn("xr-surface", "Surface cleanup raised a JNI exception");
          env->ExceptionClear();
        }
        env->Throw(pending);
        env->DeleteLocalRef(pending);
      }
    }
    if (window) {
      { std::lock_guard lock(window->lock); window->image_consumer = false; window->frame.reset(); }
      android::release(window);
    }
  }
  XrResult initialize(std::uint32_t width, std::uint32_t height) { return upload.initialize(width, height); }
  XrResult present(Swapchain& chain) {
    std::shared_ptr<const android::SurfaceFrame> frame;
    {
      std::lock_guard lock(window->lock);
      frame = window->frame;
    }
    auto serial = frame ? frame->serial : 0;
    if (initialized && serial == published && published_color == state().color_space) return XR_SUCCESS;
    auto* destination = upload.pixels();
    auto width = chain.info.width, height = chain.info.height;
    if (!frame) {
      std::memset(destination, 0, std::size_t(width) * height * 4);
    } else if (std::uint32_t(frame->width) == width && std::uint32_t(frame->height) == height &&
               frame->rgba.size() == std::size_t(width) * height * 4) {
      // Decoders convert to the swapchain's fixed extent: one copy, no sampling.
      std::memcpy(destination, frame->rgba.data(), frame->rgba.size());
    } else {
      // BufferQueue producers may resize. Snapshot one completed producer
      // buffer, with the same consumer for CPU, EGL and Vulkan publication.
      // Cache horizontal sampling offsets: division for every destination
      // pixel needlessly runs on the game's submitting thread for movies.
      if(sampled_width!=std::uint32_t(frame->width) || source_columns.size()!=width) {
        sampled_width=frame->width;source_columns.resize(width);
        for(std::uint32_t x=0;x<width;++x)source_columns[x]=(std::uint64_t(x)*frame->width/width)*4;
      }
      for (std::uint32_t y = 0; y < height; ++y) {
        auto sy = std::uint64_t(y) * frame->height / height;
        auto* row=frame->rgba.data()+sy*frame->width*4;
        for (std::uint32_t x = 0; x < width; ++x) {
          std::memcpy(destination + (std::uint64_t(y) * width + x) * 4,
              row+source_columns[x], 4);
        }
      }
    }
    auto result = upload.publish(chain);
    if (XR_FAILED(result)) return result;
    initialized = true;
    published = serial;
    published_color = state().color_space;
    return XR_SUCCESS;
  }
};

XrResult present_android_surfaces() {
  for (auto& [handle, chain] : state().swapchains)
    if (chain.android_surface) {
      auto result = chain.android_surface->present(chain);
      if (XR_FAILED(result)) return result;
    }
  return XR_SUCCESS;
}

namespace guest {
XrResult xrCreateSwapchainAndroidSurfaceKHR(XrSession handle, const XrSwapchainCreateInfo* info,
                                           XrSwapchain* out, void** surface) {
  std::lock_guard lock(state().mutex);
  if (out) *out = XR_NULL_HANDLE;
  if (surface) *surface = nullptr;
  if (!valid_handle(handle, "XrSession")) return XR_ERROR_HANDLE_INVALID;
  if (!enabled("XR_KHR_android_surface_swapchain")) return XR_ERROR_FUNCTION_UNSUPPORTED;
  // Android platform ABI: these five fields are required to be zero; the
  // runtime chooses the producer's image format and image count internally.
  if (!info || !out || !surface || info->type != XR_TYPE_SWAPCHAIN_CREATE_INFO ||
      info->format || info->sampleCount || info->faceCount || info->arraySize || info->mipCount ||
      !info->width || !info->height || std::uint64_t(info->width) * info->height > INT32_MAX / 4)
    return XR_ERROR_VALIDATION_FAILURE;
  constexpr auto known_flags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT | XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT;
  if (info->createFlags & ~known_flags) return XR_ERROR_VALIDATION_FAILURE;
  // Dynamic producers cannot satisfy the static-image contract, and a CPU
  // producer cannot provide a protected-content path.
  if (info->createFlags) return XR_ERROR_FEATURE_UNSUPPORTED;
  auto bridge = android::surface_bridge();
  if (!bridge.to_surface || !java::host_vm() || process_config().isa() != GuestIsa::a64)
    return XR_ERROR_FUNCTION_UNSUPPORTED;
  auto producer = std::make_shared<AndroidSurface>();
  producer->window = android::create_native_window(info->width, info->height);
  if (!producer->window) return XR_ERROR_OUT_OF_MEMORY;
  producer->window->image_consumer = true;
  producer->window->fixed_consumer_extent = true;
  auto result = producer->initialize(info->width, info->height);
  if (XR_FAILED(result)) return result;
  auto* env = java::host_env();
  producer->surface = bridge.to_surface(nullptr, producer->window);
  if (!producer->surface) return XR_ERROR_RUNTIME_FAILURE;
  auto global = env->NewGlobalRef(producer->surface);
  if (!global) return XR_ERROR_OUT_OF_MEMORY;
  env->DeleteLocalRef(producer->surface);
  producer->surface = global;
  producer->global_surface = true;
  auto native = *info;
  // Android WINDOW_FORMAT_RGBA_8888/RGBX_8888/RGB_565 bytes use the
  // default sRGB dataspace (ANativeWindow / BufferQueue). Preserve the encoded
  // bytes and let the compositor's sRGB image view decode them when sampled.
  native.format = VK_FORMAT_R8G8B8A8_SRGB;
  native.sampleCount = native.faceCount = native.arraySize = native.mipCount = 1;
  native.usageFlags |= XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
  result = create_swapchain(handle, &native, out, true);
  if (XR_FAILED(result)) return result;
  auto& chain = state().swapchains.at(*out);
  chain.android_surface = std::move(producer);
  result = chain.android_surface->present(chain);
  if (XR_SUCCEEDED(result)) {
    *surface = env->NewLocalRef(chain.android_surface->surface);
    if (!*surface) result = XR_ERROR_OUT_OF_MEMORY;
  }
  if (XR_FAILED(result)) {
    guest::xrDestroySwapchain(*out);
    *out = XR_NULL_HANDLE;
    return result;
  }
  log::info("xr-surface", "Created Android Surface swapchain {}x{} backed by real Vulkan images",
            info->width, info->height);
  return XR_SUCCESS;
}
}
} // namespace venture::hle::openxr
