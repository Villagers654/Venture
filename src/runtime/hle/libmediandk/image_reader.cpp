// AImageReader and AImage (AOSP frameworks/av/media/ndk/include/media/
// NdkImageReader.h, NdkImage.h): an image queue whose producer is the
// reader's ANativeWindow. MediaCodec video output reaches it as RGBA frames
// (media.cpp surface_sink); each acquired image serves them as CPU planes or
// as an AHardwareBuffer the guest imports into GLES or Vulkan. Buffers cycle
// through a pool of maxImages, as BufferQueue's slots do, so consumers that
// cache imports per buffer see a fixed set.
#include "android/errors.h"
#include "hle/libandroid/parts.h"
#include "android/handles.h"
#include "android/hardware_buffer.h"
#include "android/native_window.h"
#include "core/address_space.h"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <thread>
#include <vector>

namespace venture::hle {
namespace {
using namespace android_hle;
// NdkMediaError.h.
constexpr int unsupported = -10002, invalid_object = -10003, invalid_parameter = -10004,
              no_buffer_available = -30001, max_images_acquired = -30002;
// NdkImage.h AIMAGE_FORMATS.
constexpr std::int32_t format_rgba = 1, format_rgbx = 2, format_yuv420 = 0x23, format_private = 0x22;

struct Reader {
  struct Slot {
    GuestAddr buffer = 0;  // one AHardwareBuffer reference
    bool busy = false;
  };
  std::int32_t width = 0, height = 0, format = 0, max_images = 0;
  std::uint64_t usage = 0;
  android::NativeWindow* window = nullptr;  // one reference while the reader lives
  std::mutex mutex;
  std::condition_variable_any available;
  std::deque<std::shared_ptr<const android::SurfaceFrame>> queue;
  std::vector<Slot> slots;
  std::int32_t acquired = 0;
  std::uint64_t pending = 0;  // frames not yet announced to the listener
  GuestAddr listener = 0, context = 0;
  std::jthread notifier;
  ~Reader() {
    for (auto& slot : slots)
      if (slot.buffer) android::release_hardware_buffer(slot.buffer);
  }
  bool gpu() const { return format == format_private || (usage & android::ahb::gpu_mask); }
};
android::Handles<Reader> readers("AImageReader");

struct Image {
  std::shared_ptr<Reader> reader;
  std::shared_ptr<const android::SurfaceFrame> frame;
  std::mutex mutex;
  std::size_t slot = SIZE_MAX;  // pool slot holding the frame, once written
  GuestAddr planes = 0;         // CPU plane copy, once requested
  std::uint64_t planes_size = 0;
  ~Image() {
    if (planes) AddressSpace::get().free(planes);
  }
};
android::Handles<Image> images("AImage");

// Runs on the decoder thread that published the frame, with the window unlocked.
void frame_arrived(const std::weak_ptr<Reader>& weak) {
  auto r = weak.lock();
  if (!r) return;
  std::shared_ptr<const android::SurfaceFrame> frame;
  {
    std::lock_guard lock(r->window->lock);
    frame = r->window->frame;
  }
  if (!frame) return;
  {
    std::lock_guard lock(r->mutex);
    if (!r->queue.empty() && r->queue.back() == frame) return;
    r->queue.push_back(std::move(frame));
    // A full queue drops its oldest frame, as an asynchronous BufferQueue does.
    while (r->queue.size() > std::size_t(std::max(1, r->max_images))) r->queue.pop_front();
    ++r->pending;
  }
  r->available.notify_all();
}
// onImageAvailable runs on the reader's own thread, as on Android's looper
// thread, so a listener may acquire or block without stalling the decoder.
// The thread keeps its reader alive; AImageReader_delete stops it.
void start_notifier(const std::shared_ptr<Reader>& reader) {
  reader->notifier = std::jthread([reader](std::stop_token stop) {
    auto& r = *reader;
    for (;;) {
      GuestAddr fn = 0, context = 0;
      {
        std::unique_lock lock(r.mutex);
        if (!r.available.wait(lock, stop, [&] { return r.pending && r.listener; })) return;
        --r.pending;
        fn = r.listener;
        context = r.context;
      }
      GuestThread::adopt_current().call(fn, {context, h2g(&r)});
    }
  });
}

int create(std::int32_t width, std::int32_t height, std::int32_t format, std::uint64_t usage,
           std::int32_t max_images, GuestAddr* out) {
  if (!out) return invalid_parameter;
  *out = 0;
  if (width < 1 || height < 1 || max_images < 1) return invalid_parameter;
  if (format != format_rgba && format != format_rgbx && format != format_yuv420 && format != format_private) {
    log::warn("media", "AImageReader format {:#x} is not supported", format);
    return unsupported;
  }
  auto* window = android::create_native_window(width, height);
  if (!window) return invalid_parameter;
  auto* p = readers.create();
  auto r = readers.get(p);
  r->width = width;
  r->height = height;
  r->format = format;
  r->usage = usage;
  r->max_images = max_images;
  r->window = window;
  {
    std::lock_guard lock(window->lock);
    window->image_consumer = true;
    window->frame_available = [weak = std::weak_ptr<Reader>(r)] { frame_arrived(weak); };
  }
  start_notifier(r);
  log::info("media", "AImageReader {}x{} format={:#x} usage={:#x} maxImages={}", width, height, format, usage,
            max_images);
  *out = h2g(p);
  return 0;
}
int reader_new_usage(std::int32_t width, std::int32_t height, std::int32_t format, std::uint64_t usage,
                     std::int32_t max_images, GuestAddr* out) {
  return create(width, height, format, usage, max_images, out);
}
int reader_new(std::int32_t width, std::int32_t height, std::int32_t format, std::int32_t max_images, GuestAddr* out) {
  // AImageReader_new: CPU reads, except PRIVATE images, which only the GPU reads.
  return create(width, height, format, format == format_private ? android::ahb::gpu_sampled_image : 3, max_images,
                out);
}
void reader_delete(Reader* p) {
  auto r = readers.get(p);
  if (!r) return;
  readers.erase(p);
  {
    std::lock_guard lock(r->window->lock);
    r->window->frame_available = nullptr;
    r->window->image_consumer = false;
  }
  r->notifier.request_stop();
  // A listener deleting its own reader cannot wait for itself.
  if (r->notifier.get_id() == std::this_thread::get_id())
    r->notifier.detach();
  else if (r->notifier.joinable())
    r->notifier.join();
  android::release(r->window);
}
int reader_window(Reader* p, GuestAddr* out) {
  auto r = readers.get(p);
  if (!out) return invalid_parameter;
  if (!r) return invalid_object;
  *out = h2g(r->window);  // owned by the reader, as on Android
  return 0;
}
template <std::int32_t Reader::* Field>
int reader_value(Reader* p, std::int32_t* out) {
  auto r = readers.get(p);
  if (!out) return invalid_parameter;
  if (!r) return invalid_object;
  *out = r.get()->*Field;
  return 0;
}
int acquire(Reader* p, GuestAddr* out, bool latest) {
  if (!out) return invalid_parameter;
  *out = 0;
  auto r = readers.get(p);
  if (!r) return invalid_object;
  std::shared_ptr<const android::SurfaceFrame> frame;
  {
    std::lock_guard lock(r->mutex);
    if (r->acquired >= r->max_images) return max_images_acquired;
    if (r->queue.empty()) return no_buffer_available;
    if (latest) {
      frame = std::move(r->queue.back());
      r->queue.clear();
    } else {
      frame = std::move(r->queue.front());
      r->queue.pop_front();
    }
    ++r->acquired;
  }
  auto* image = images.create();
  auto i = images.get(image);
  i->reader = std::move(r);
  i->frame = std::move(frame);
  *out = h2g(image);
  return 0;
}
int acquire_next(Reader* p, GuestAddr* out) { return acquire(p, out, false); }
int acquire_latest(Reader* p, GuestAddr* out) { return acquire(p, out, true); }
int acquire_next_async(Reader* p, GuestAddr* out, int* fence) {
  if (fence) *fence = -1;  // frames are complete when queued
  return acquire(p, out, false);
}
int acquire_latest_async(Reader* p, GuestAddr* out, int* fence) {
  if (fence) *fence = -1;
  return acquire(p, out, true);
}
int set_listener(Reader* p, const GuestAddr* listener) {
  auto r = readers.get(p);
  if (!r) return invalid_object;
  {
    std::lock_guard lock(r->mutex);
    // AImageReader_ImageListener { void* context; void (*onImageAvailable)(void*, AImageReader*); }
    r->context = listener ? listener[0] : 0;
    r->listener = listener ? listener[1] : 0;
    r->pending = r->listener ? r->queue.size() : 0;
  }
  r->available.notify_all();
  return 0;
}
int set_removed_listener(Reader* p, const GuestAddr*) {
  // Buffers stay in the pool until the reader is deleted; none is removed earlier.
  return readers.get(p) ? 0 : invalid_object;
}

void image_delete(Image* p) {
  auto i = images.get(p);
  if (!i) return;
  images.erase(p);
  std::lock_guard lock(i->reader->mutex);
  --i->reader->acquired;
  if (i->slot != SIZE_MAX) i->reader->slots[i->slot].busy = false;
}
void image_delete_async(Image* p, int fence) {
  (void)fence;  // the guest's release fence; readers do not reuse a buffer before AImage_delete
  image_delete(p);
}
int image_width(Image* p, std::int32_t* out) {
  auto i = images.get(p);
  if (!out) return invalid_parameter;
  if (!i) return invalid_object;
  *out = i->frame->width;
  return 0;
}
int image_height(Image* p, std::int32_t* out) {
  auto i = images.get(p);
  if (!out) return invalid_parameter;
  if (!i) return invalid_object;
  *out = i->frame->height;
  return 0;
}
int image_format(Image* p, std::int32_t* out) {
  auto i = images.get(p);
  if (!out) return invalid_parameter;
  if (!i) return invalid_object;
  *out = i->reader->format;
  return 0;
}
int image_timestamp(Image* p, std::int64_t* out) {
  auto i = images.get(p);
  if (!out) return invalid_parameter;
  if (!i) return invalid_object;
  *out = i->frame->timestamp_ns;
  return 0;
}
int image_crop(Image* p, android::Rect* out) {
  auto i = images.get(p);
  if (!out) return invalid_parameter;
  if (!i) return invalid_object;
  *out = {0, 0, i->frame->width, i->frame->height};
  return 0;
}
std::int32_t plane_count(std::int32_t format) {
  return format == format_yuv420 ? 3 : format == format_private ? 0 : 1;
}
int image_planes(Image* p, std::int32_t* out) {
  auto i = images.get(p);
  if (!out) return invalid_parameter;
  if (!i) return invalid_object;
  *out = plane_count(i->reader->format);
  return 0;
}
// Plane layout: RGBA/RGBX one interleaved plane; YUV_420_888 a full Y plane
// then quarter-size U and V planes (BT.601 limited range, as video decoders output).
int plane_strides(const Image& i, int plane, std::int32_t* pixel, std::int32_t* row) {
  const auto format = i.reader->format;
  if (format == format_private) return unsupported;
  if (plane < 0 || plane >= plane_count(format)) return invalid_parameter;
  const auto w = i.frame->width;
  if (format == format_yuv420) {
    *pixel = 1;
    *row = plane ? (w + 1) / 2 : w;
  } else {
    *pixel = 4;
    *row = w * 4;
  }
  return 0;
}
int image_pixel_stride(Image* p, int plane, std::int32_t* out) {
  auto i = images.get(p);
  if (!out) return invalid_parameter;
  if (!i) return invalid_object;
  std::int32_t pixel = 0, row = 0;
  if (auto e = plane_strides(*i, plane, &pixel, &row)) return e;
  *out = pixel;
  return 0;
}
int image_row_stride(Image* p, int plane, std::int32_t* out) {
  auto i = images.get(p);
  if (!out) return invalid_parameter;
  if (!i) return invalid_object;
  std::int32_t pixel = 0, row = 0;
  if (auto e = plane_strides(*i, plane, &pixel, &row)) return e;
  *out = row;
  return 0;
}
// Fills guest memory with the image's planes on first use.
bool fill_planes(Image& i) {
  if (i.planes) return true;
  const auto& f = *i.frame;
  const std::size_t w = f.width, h = f.height, cw = (w + 1) / 2, ch = (h + 1) / 2;
  const bool yuv = i.reader->format == format_yuv420;
  const std::uint64_t size = yuv ? w * h + 2 * cw * ch : w * h * 4;
  const auto address = AddressSpace::get().allocate(size, kProtRead | kProtWrite, "[aimage-planes]");
  if (!address) return false;
  auto* out = g2h<std::uint8_t>(address);
  if (!yuv) {
    std::memcpy(out, f.rgba.data(), size);
    if (i.reader->format == format_rgbx)
      for (std::size_t n = 3; n < size; n += 4) out[n] = 0xff;
  } else {
    auto* u = out + w * h;
    auto* v = u + cw * ch;
    auto at = [&](std::size_t x, std::size_t y) { return f.rgba.data() + (y * w + x) * 4; };
    for (std::size_t y = 0; y < h; ++y)
      for (std::size_t x = 0; x < w; ++x) {
        const auto* c = at(x, y);
        out[y * w + x] = std::uint8_t((66 * c[0] + 129 * c[1] + 25 * c[2] + 128) / 256 + 16);
      }
    for (std::size_t y = 0; y < ch; ++y)
      for (std::size_t x = 0; x < cw; ++x) {
        int r = 0, g = 0, b = 0, n = 0;
        for (std::size_t dy = 0; dy < 2 && 2 * y + dy < h; ++dy)
          for (std::size_t dx = 0; dx < 2 && 2 * x + dx < w; ++dx, ++n) {
            const auto* c = at(2 * x + dx, 2 * y + dy);
            r += c[0];
            g += c[1];
            b += c[2];
          }
        r /= n;
        g /= n;
        b /= n;
        u[y * cw + x] = std::uint8_t((-38 * r - 74 * g + 112 * b + 128) / 256 + 128);
        v[y * cw + x] = std::uint8_t((112 * r - 94 * g - 18 * b + 128) / 256 + 128);
      }
  }
  i.planes = address;
  i.planes_size = size;
  return true;
}
int image_plane_data(Image* p, int plane, GuestAddr* data, std::int32_t* length) {
  auto i = images.get(p);
  if (!data || !length) return invalid_parameter;
  if (!i) return invalid_object;
  std::int32_t pixel = 0, row = 0;
  if (auto e = plane_strides(*i, plane, &pixel, &row)) return e;
  std::lock_guard lock(i->mutex);
  if (!fill_planes(*i)) return unsupported;
  const auto& f = *i->frame;
  const std::uint64_t luma = std::uint64_t(f.width) * f.height, chroma = ((f.width + 1) / 2) * ((f.height + 1) / 2);
  if (i->reader->format != format_yuv420) {
    *data = i->planes;
    *length = std::int32_t(i->planes_size);
  } else {
    *data = i->planes + (plane == 0 ? 0 : plane == 1 ? luma : luma + chroma);
    *length = std::int32_t(plane ? chroma : luma);
  }
  return 0;
}
int image_buffer(Image* p, GuestAddr* out) {
  if (!out) return invalid_parameter;
  *out = 0;
  auto i = images.get(p);
  if (!i) return invalid_object;
  std::lock_guard lock(i->mutex);
  auto& r = *i->reader;
  const auto& f = *i->frame;
  if (i->slot == SIZE_MAX) {
    std::unique_lock reader_lock(r.mutex);
    // A free slot with a buffer of this frame's size, else a new or resized one.
    auto fits = [&](const Reader::Slot& s) {
      auto b = android::hardware_buffer(s.buffer);
      return b && b->desc.width == std::uint32_t(f.width) && b->desc.height == std::uint32_t(f.height);
    };
    std::size_t slot = SIZE_MAX;
    for (std::size_t n = 0; n < r.slots.size() && slot == SIZE_MAX; ++n)
      if (!r.slots[n].busy && fits(r.slots[n])) slot = n;
    for (std::size_t n = 0; n < r.slots.size() && slot == SIZE_MAX; ++n)
      if (!r.slots[n].busy) slot = n;
    if (slot == SIZE_MAX) {
      r.slots.emplace_back();
      slot = r.slots.size() - 1;
    }
    auto& s = r.slots[slot];
    s.busy = true;
    if (!fits(s)) {
      if (s.buffer) android::release_hardware_buffer(std::exchange(s.buffer, 0));
      const std::uint64_t usage =
          r.gpu() ? (r.usage & (android::ahb::gpu_sampled_image | android::ahb::gpu_framebuffer)) |
                        android::ahb::gpu_sampled_image
                  : 0x33;  // CPU read and write, often
      android::HardwareBufferDesc desc{std::uint32_t(f.width), std::uint32_t(f.height), 1, format_rgba, usage, 0, 0,
                                       0};
      if (android::allocate_hardware_buffer(desc, &s.buffer) != 0) {
        s.busy = false;
        log::warn("media", "AImage_getHardwareBuffer: no {}x{} buffer with usage {:#x}", f.width, f.height, usage);
        return unsupported;
      }
    }
    i->slot = slot;
    reader_lock.unlock();
    if (!android::write_hardware_buffer(s.buffer, f.rgba)) {
      log::warn("media", "AImage_getHardwareBuffer: writing the {}x{} frame failed", f.width, f.height);
      return unsupported;
    }
  }
  std::lock_guard reader_lock(r.mutex);
  *out = r.slots[i->slot].buffer;  // not acquired for the caller, as on Android
  return 0;
}
}  // namespace

void register_image_reader(HleLibrary& lib) {
#define API(name, fn) add<&fn>(lib, name)
  API("AImageReader_new", reader_new);
  API("AImageReader_newWithUsage", reader_new_usage);
  API("AImageReader_delete", reader_delete);
  API("AImageReader_getWindow", reader_window);
  API("AImageReader_getWidth", reader_value<&Reader::width>);
  API("AImageReader_getHeight", reader_value<&Reader::height>);
  API("AImageReader_getFormat", reader_value<&Reader::format>);
  API("AImageReader_getMaxImages", reader_value<&Reader::max_images>);
  API("AImageReader_acquireNextImage", acquire_next);
  API("AImageReader_acquireLatestImage", acquire_latest);
  API("AImageReader_acquireNextImageAsync", acquire_next_async);
  API("AImageReader_acquireLatestImageAsync", acquire_latest_async);
  API("AImageReader_setImageListener", set_listener);
  API("AImageReader_setBufferRemovedListener", set_removed_listener);
  API("AImage_delete", image_delete);
  API("AImage_deleteAsync", image_delete_async);
  API("AImage_getWidth", image_width);
  API("AImage_getHeight", image_height);
  API("AImage_getFormat", image_format);
  API("AImage_getTimestamp", image_timestamp);
  API("AImage_getCropRect", image_crop);
  API("AImage_getNumberOfPlanes", image_planes);
  API("AImage_getPlanePixelStride", image_pixel_stride);
  API("AImage_getPlaneRowStride", image_row_stride);
  API("AImage_getPlaneData", image_plane_data);
  API("AImage_getHardwareBuffer", image_buffer);
#undef API
}
}  // namespace venture::hle
