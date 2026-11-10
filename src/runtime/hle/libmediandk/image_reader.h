// Host access to the NDK image reader (image_reader.cpp) for the framework's
// android.media.ImageReader: the same queue, buffer pool and images, so Java
// and NDK consumers of a decoder see one implementation. Handles are the
// AImageReader* and AImage* addresses; errors are NdkMediaError values.
#pragma once
#include "core/guest.h"
#include <cstdint>

namespace venture::hle::image_reader {
int create(std::int32_t width, std::int32_t height, std::int32_t format, std::uint64_t usage,
           std::int32_t max_images, GuestAddr* reader);
void destroy(GuestAddr reader);
// The reader's ANativeWindow, owned by the reader.
GuestAddr window(GuestAddr reader);
// Starts counting frames for the Java listener from those already queued.
void listen(GuestAddr reader);
// Waits up to timeout_ms for a frame not yet announced: 1 when one arrived, 0
// on timeout, negative when the reader is gone.
int wait(GuestAddr reader, int timeout_ms);
int acquire(GuestAddr reader, bool latest, GuestAddr* image);
void release(GuestAddr image);
struct ImageInfo {
  std::int32_t width = 0, height = 0, format = 0, planes = 0;
  std::int64_t timestamp_ns = 0;
};
int info(GuestAddr image, ImageInfo* out);
int plane(GuestAddr image, int index, GuestAddr* data, std::int32_t* length, std::int32_t* pixel_stride,
          std::int32_t* row_stride);
// The image's AHardwareBuffer, not acquired for the caller.
int hardware_buffer(GuestAddr image, GuestAddr* buffer);
}  // namespace venture::hle::image_reader
