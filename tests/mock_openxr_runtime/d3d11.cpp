#include "d3d11.h"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr_platform.h>
#include "xr/capture.h"
#include <algorithm>
#include <cstdlib>
#include <deque>
#include <format>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace mock::d3d11 {
namespace {
ID3D11Device* device{};
ID3D11DeviceContext4* context{};
ID3D11Fence* fence{};
HANDLE fence_event{};
std::uint64_t fence_value = 0;
std::deque<std::pair<std::uint32_t, std::uint64_t>> pending;  // frame, fence value
constexpr std::uint32_t content_slots = 4;
struct ContentRing {
  ID3D11Texture2D* staging[content_slots]{};
  std::uint32_t frames[content_slots]{};
  std::uint32_t width = 0, format = 0, filled = 0, next = 0;
} content;
void release_content() {
  for (auto*& texture : content.staging)
    if (texture) texture->Release(), texture = nullptr;
  content = {};
}

template <class T> void release(T*& object) {
  if (object) object->Release();
  object = nullptr;
}
}  // namespace

bool available() { return true; }
bool active() { return device != nullptr; }

bool bind(const void* next, XrResult* result) {
  const auto* binding = static_cast<const XrGraphicsBindingD3D11KHR*>(next);
  if (!binding || binding->type != XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) return false;
  *result = XR_ERROR_GRAPHICS_DEVICE_INVALID;
  if (!binding->device) return true;
  ID3D11Device5* device5{};
  ID3D11DeviceContext* immediate{};
  binding->device->GetImmediateContext(&immediate);
  if (FAILED(binding->device->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(&device5))) ||
      FAILED(immediate->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void**>(&context))) ||
      FAILED(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, __uuidof(ID3D11Fence), reinterpret_cast<void**>(&fence)))) {
    std::cerr << "[mock-xr] D3D11 binding needs ID3D11Device5 fences\n";
    release(device5);
    release(immediate);
    release(context);
    return true;
  }
  release(immediate);
  release(device5);
  fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  device = binding->device;
  device->AddRef();
  fence_value = 0;
  pending.clear();
  *result = XR_SUCCESS;
  return true;
}

void unbind() {
  if (fence && fence_value) {
    fence->SetEventOnCompletion(fence_value, fence_event);
    WaitForSingleObject(fence_event, 2000);
  }
  pending.clear();
  release_content();
  release(fence);
  release(context);
  release(device);
  if (fence_event) CloseHandle(fence_event);
  fence_event = nullptr;
}

XrResult requirements(XrInstance, XrSystemId, void* out) {
  auto* r = static_cast<XrGraphicsRequirementsD3D11KHR*>(out);
  if (!r || r->type != XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR) return XR_ERROR_VALIDATION_FAILURE;
  IDXGIFactory1* factory{};
  IDXGIAdapter1* adapter{};
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) ||
      factory->EnumAdapters1(0, &adapter) == DXGI_ERROR_NOT_FOUND) {
    release(factory);
    return XR_ERROR_RUNTIME_FAILURE;
  }
  DXGI_ADAPTER_DESC1 desc{};
  adapter->GetDesc1(&desc);
  r->adapterLuid = desc.AdapterLuid;
  r->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
  release(adapter);
  release(factory);
  return XR_SUCCESS;
}

std::vector<std::int64_t> formats() {
  return {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
          DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_D32_FLOAT,
          DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_D16_UNORM};
}

XrResult create_images(const XrSwapchainCreateInfo& info, int count, std::vector<void*>& images) {
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = info.width;
  desc.Height = info.height;
  desc.MipLevels = info.mipCount;
  desc.ArraySize = info.arraySize * info.faceCount;
  desc.Format = static_cast<DXGI_FORMAT>(info.format);
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  if (info.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
  if (info.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) desc.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
  if (info.usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
  if (info.usageFlags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
  if (info.faceCount == 6) desc.MiscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;
  for (int i = 0; i < count; ++i) {
    ID3D11Texture2D* texture{};
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture))) {
      destroy_images(images);
      return XR_ERROR_OUT_OF_MEMORY;
    }
    images.push_back(texture);
  }
  return XR_SUCCESS;
}

void destroy_images(std::vector<void*>& images) {
  for (auto* image : images) static_cast<ID3D11Texture2D*>(image)->Release();
  images.clear();
}

XrResult enumerate_images(const std::vector<void*>& images, XrSwapchainImageBaseHeader* out) {
  auto* list = reinterpret_cast<XrSwapchainImageD3D11KHR*>(out);
  for (std::size_t i = 0; i < images.size(); ++i) {
    if (list[i].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR) return XR_ERROR_VALIDATION_FAILURE;
    list[i].texture = static_cast<ID3D11Texture2D*>(images[i]);
  }
  return XR_SUCCESS;
}

// XR_KHR_D3D11_enable: the application synchronizes its immediate context with
// xrEndFrame, which is where the fence is signaled; waits use only the fence.
void fence_frame(std::uint32_t frame) {
  if (!fence || FAILED(context->Signal(fence, ++fence_value))) return;
  context->Flush();
  pending.emplace_back(frame, fence_value);
}

std::uint64_t due(std::uint32_t next) {
  std::uint64_t value = 0;
  while (!pending.empty() && pending.front().first + 2 <= next) {
    value = pending.front().second;
    pending.pop_front();
  }
  return value && fence->GetCompletedValue() < value ? value : 0;
}

std::int64_t wait(std::uint64_t value, std::uint64_t bound_ns) {
  if (!value) return 0;
  LARGE_INTEGER start, end, frequency;
  QueryPerformanceCounter(&start);
  fence->SetEventOnCompletion(value, fence_event);
  if (WaitForSingleObject(fence_event, DWORD(bound_ns / 1'000'000)) != WAIT_OBJECT_0) {
    static unsigned reports = 0;
    if (reports++ < 8) std::cerr << "[mock-xr] D3D11 frame before last not GPU-complete within the bound\n";
  }
  QueryPerformanceCounter(&end);
  QueryPerformanceFrequency(&frequency);
  return (end.QuadPart - start.QuadPart) * 1'000'000'000 / frequency.QuadPart;
}

// Diagnostics only: synchronous readback on the application's xrEndFrame thread.
// Capture frames are outside VENTURE_MOCK_UNPACED windows, so measured frames never pay it.
void capture(void* image, const XrSwapchainCreateInfo& info, const XrSwapchainSubImage& sub,
             const std::filesystem::path& path) {
  const auto format = static_cast<DXGI_FORMAT>(info.format);
  const bool bgra = format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
  if (!bgra && format != DXGI_FORMAT_R8G8B8A8_UNORM && format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
    throw std::runtime_error(std::format("D3D11 capture of DXGI format {} is unsupported", info.format));
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = info.width;
  desc.Height = info.height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_STAGING;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D* staging{};
  if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging)))
    throw std::runtime_error("D3D11 staging texture failed");
  context->CopySubresourceRegion(staging, 0, 0, 0, 0, static_cast<ID3D11Texture2D*>(image),
                                 D3D11CalcSubresource(0, sub.imageArrayIndex, info.mipCount), nullptr);
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
    staging->Release();
    throw std::runtime_error("D3D11 staging map failed");
  }
  std::vector<unsigned char> pixels(std::size_t(info.width) * info.height * 4);
  for (std::uint32_t y = 0; y < info.height; ++y) {
    const auto* row = static_cast<const unsigned char*>(mapped.pData) + std::size_t(y) * mapped.RowPitch;
    auto* to = pixels.data() + std::size_t(y) * info.width * 4;
    for (std::uint32_t x = 0; x < info.width; ++x) {
      const auto* p = row + x * 4;
      to[x * 4 + 0] = bgra ? p[2] : p[0];
      to[x * 4 + 1] = p[1];
      to[x * 4 + 2] = bgra ? p[0] : p[2];
      to[x * 4 + 3] = p[3];
    }
  }
  context->Unmap(staging, 0);
  staging->Release();
  venture::xr::write_eye(path, info.width, info.height, pixels, sub.imageRect);
}
bool content_hash_enabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("VENTURE_MOCK_CONTENT_HASH");
    return value && std::string_view(value) == "1";
  }();
  return enabled;
}

ContentSample sample_content(void* image, const XrSwapchainCreateInfo& info, const XrSwapchainSubImage& sub,
                             const SignatureWindow& window, std::uint32_t frame) {
  const auto width = static_cast<std::uint32_t>(sub.imageRect.extent.width);
  const auto height = static_cast<std::uint32_t>(sub.imageRect.extent.height);
  if (!device || width == 0 || height < 4) return {};
  if (content.width != width || content.format != static_cast<std::uint32_t>(info.format)) {
    release_content();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = 3;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = static_cast<DXGI_FORMAT>(info.format);
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (auto*& texture : content.staging)
      if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture))) {
        release_content();
        return {};
      }
    content.width = width;
    content.format = static_cast<std::uint32_t>(info.format);
  }
  ContentSample result;
  const auto slot = content.next;
  if (content.filled == content_slots) {
    // The oldest copy was queued three frames ago; mapping it rarely waits.
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context->Map(content.staging[slot], 0, D3D11_MAP_READ, 0, &mapped))) {
      const auto bytes = std::min<std::size_t>(mapped.RowPitch,
          std::size_t(width) * (info.format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4));
      std::uint64_t hash = 1469598103934665603ull;
      for (std::uint32_t row = 0; row < 3; ++row) {
        const auto* p = static_cast<const unsigned char*>(mapped.pData) + std::size_t(row) * mapped.RowPitch;
        for (std::size_t i = 0; i < bytes; ++i) hash = (hash ^ p[i]) * 1099511628211ull;
      }
      const auto* base = static_cast<const unsigned char*>(mapped.pData);
      const unsigned char* const rows[3]{base, base + mapped.RowPitch, base + 2 * std::size_t(mapped.RowPitch)};
      const auto signature = scene_signature(rows, window, info.format == DXGI_FORMAT_R16G16B16A16_FLOAT);
      context->Unmap(content.staging[slot], 0);
      result = {content.frames[slot], hash, signature};
    }
  } else {
    ++content.filled;
  }
  const auto subresource = D3D11CalcSubresource(0, sub.imageArrayIndex, info.mipCount);
  for (std::uint32_t row = 0; row < 3; ++row) {
    const auto y = static_cast<UINT>(sub.imageRect.offset.y) + window.rows[row];
    const D3D11_BOX box{static_cast<UINT>(sub.imageRect.offset.x), y, 0,
                        static_cast<UINT>(sub.imageRect.offset.x) + width, y + 1, 1};
    context->CopySubresourceRegion(content.staging[slot], 0, 0, row, 0, static_cast<ID3D11Texture2D*>(image),
                                   subresource, &box);
  }
  content.frames[slot] = frame;
  content.next = (slot + 1) % content_slots;
  return result;
}
}  // namespace mock::d3d11
#else
namespace mock::d3d11 {
bool available() { return false; }
bool active() { return false; }
bool bind(const void*, XrResult*) { return false; }
void unbind() {}
XrResult requirements(XrInstance, XrSystemId, void*) { return XR_ERROR_FUNCTION_UNSUPPORTED; }
std::vector<std::int64_t> formats() { return {}; }
XrResult create_images(const XrSwapchainCreateInfo&, int, std::vector<void*>&) { return XR_ERROR_RUNTIME_FAILURE; }
void destroy_images(std::vector<void*>&) {}
XrResult enumerate_images(const std::vector<void*>&, XrSwapchainImageBaseHeader*) { return XR_ERROR_RUNTIME_FAILURE; }
void fence_frame(std::uint32_t) {}
std::uint64_t due(std::uint32_t) { return 0; }
std::int64_t wait(std::uint64_t, std::uint64_t) { return 0; }
void capture(void*, const XrSwapchainCreateInfo&, const XrSwapchainSubImage&, const std::filesystem::path&) {}
bool content_hash_enabled() { return false; }
ContentSample sample_content(void*, const XrSwapchainCreateInfo&, const XrSwapchainSubImage&, const SignatureWindow&,
                             std::uint32_t) {
  return {};
}
}
#endif
