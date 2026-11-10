// android.media.ImageReader and android.hardware.HardwareBuffer natives: the
// framework's ImageReader drives the NDK image reader (hle/libmediandk), and a
// HardwareBuffer object holds one AHardwareBuffer reference.
#include "java/natives/media_bridge.h"
#include "java/internal.h"
#include "android/hardware_buffer.h"
#include "android/native_window.h"
#include "core/address_space.h"
#include "hle/libmediandk/image_reader.h"
#include "common/log.h"
#include <iterator>

namespace venture::java {
namespace {
namespace ir = hle::image_reader;
constexpr int max_images_acquired = -30002, no_buffer_available = -30001;

jlong JNICALL reader_create(JNIEnv*, jclass, jint width, jint height, jint format, jlong usage, jint max_images) {
  GuestAddr reader = 0;
  if (ir::create(width, height, format, static_cast<std::uint64_t>(usage), max_images, &reader) != 0) return 0;
  return static_cast<jlong>(reader);
}
void JNICALL reader_close(JNIEnv*, jclass, jlong reader) { ir::destroy(static_cast<GuestAddr>(reader)); }
jlong JNICALL reader_window(JNIEnv*, jclass, jlong reader) {
  const auto window = ir::window(static_cast<GuestAddr>(reader));
  auto* w = android::native_window_from_guest(window);
  if (!w) return 0;
  android::acquire(w);  // the Surface's own reference
  return static_cast<jlong>(window);
}
void JNICALL reader_listen(JNIEnv*, jclass, jlong reader) { ir::listen(static_cast<GuestAddr>(reader)); }
jint JNICALL reader_wait(JNIEnv*, jclass, jlong reader, jint timeout_ms) {
  const int result = ir::wait(static_cast<GuestAddr>(reader), timeout_ms);
  return result > 0 ? 1 : result < 0 ? -1 : 0;
}
jlong JNICALL reader_acquire(JNIEnv*, jclass, jlong reader, jboolean latest) {
  GuestAddr image = 0;
  const int result = ir::acquire(static_cast<GuestAddr>(reader), latest, &image);
  if (result == max_images_acquired) return -1;
  if (result == no_buffer_available || result != 0) return 0;
  return static_cast<jlong>(image);
}
void JNICALL image_release(JNIEnv*, jclass, jlong image) { ir::release(static_cast<GuestAddr>(image)); }
jlongArray JNICALL image_info(JNIEnv* env, jclass, jlong image) {
  ir::ImageInfo info;
  if (ir::info(static_cast<GuestAddr>(image), &info) != 0) {
    throw_java(env, "java/lang/IllegalStateException", "Image is no longer valid");
    return nullptr;
  }
  const jlong values[]{info.width, info.height, info.format, info.planes, info.timestamp_ns};
  auto result = env->NewLongArray(jsize(std::size(values)));
  if (result) env->SetLongArrayRegion(result, 0, jsize(std::size(values)), values);
  return result;
}
jobject JNICALL image_plane(JNIEnv* env, jclass, jlong image, jint index, jintArray strides) {
  GuestAddr data = 0;
  std::int32_t length = 0, pixel = 0, row = 0;
  if (ir::plane(static_cast<GuestAddr>(image), index, &data, &length, &pixel, &row) != 0 || !data) return nullptr;
  const jint values[]{pixel, row};
  if (strides) env->SetIntArrayRegion(strides, 0, 2, values);
  return env->NewDirectByteBuffer(g2h<void>(data), length);
}
jlong JNICALL image_buffer(JNIEnv*, jclass, jlong image) {
  GuestAddr buffer = 0;
  return ir::hardware_buffer(static_cast<GuestAddr>(image), &buffer) == 0 ? static_cast<jlong>(buffer) : 0;
}

android::HardwareBufferDesc describe(jint width, jint height, jint format, jint layers, jlong usage) {
  return {std::uint32_t(width), std::uint32_t(height), std::uint32_t(layers), std::uint32_t(format),
          static_cast<std::uint64_t>(usage), 0, 0, 0};
}
jlong JNICALL buffer_create(JNIEnv*, jclass, jint width, jint height, jint format, jint layers, jlong usage) {
  GuestAddr buffer = 0;
  return android::allocate_hardware_buffer(describe(width, height, format, layers, usage), &buffer) == 0
             ? static_cast<jlong>(buffer) : 0;
}
jboolean JNICALL buffer_supported(JNIEnv*, jclass, jint width, jint height, jint format, jint layers, jlong usage) {
  return android::hardware_buffer_supported(describe(width, height, format, layers, usage));
}
void JNICALL buffer_acquire(JNIEnv*, jclass, jlong buffer) { android::acquire_hardware_buffer(static_cast<GuestAddr>(buffer)); }
void JNICALL buffer_release(JNIEnv*, jclass, jlong buffer) { android::release_hardware_buffer(static_cast<GuestAddr>(buffer)); }
jlong JNICALL buffer_describe(JNIEnv*, jclass, jlong buffer, jint field) {
  auto b = android::hardware_buffer(static_cast<GuestAddr>(buffer));
  if (!b) return 0;
  switch (field) {
    case 0: return b->desc.width;
    case 1: return b->desc.height;
    case 2: return b->desc.format;
    case 3: return b->desc.layers;
    case 4: return static_cast<jlong>(b->desc.usage);
    default: return buffer;  // getId: unique while the buffer lives
  }
}
}  // namespace

void register_image_readers(JNIEnv* env) {
#define METHOD(n, s, f) {const_cast<char*>(n), const_cast<char*>(s), reinterpret_cast<void*>(f)}
  auto cls = env->FindClass("android/media/ImageReader");
  if (!cls) return;
  const JNINativeMethod reader_methods[]{
      METHOD("nativeCreate", "(IIIJI)J", reader_create), METHOD("nativeClose", "(J)V", reader_close),
      METHOD("nativeWindow", "(J)J", reader_window), METHOD("nativeListen", "(J)V", reader_listen),
      METHOD("nativeWait", "(JI)I", reader_wait), METHOD("nativeAcquire", "(JZ)J", reader_acquire),
      METHOD("nativeRelease", "(J)V", image_release), METHOD("nativeInfo", "(J)[J", image_info),
      METHOD("nativePlane", "(JI[I)Ljava/nio/ByteBuffer;", image_plane),
      METHOD("nativeHardwareBuffer", "(J)J", image_buffer)};
  env->RegisterNatives(cls, reader_methods, jint(std::size(reader_methods)));
  env->DeleteLocalRef(cls);
  if (env->ExceptionCheck()) return;
  cls = env->FindClass("android/hardware/HardwareBuffer");
  if (!cls) return;
  const JNINativeMethod buffer_methods[]{
      METHOD("nativeCreate", "(IIIIJ)J", buffer_create), METHOD("nativeSupported", "(IIIIJ)Z", buffer_supported),
      METHOD("nativeAcquire", "(J)V", buffer_acquire), METHOD("nativeRelease", "(J)V", buffer_release),
      METHOD("nativeDescribe", "(JI)J", buffer_describe)};
  env->RegisterNatives(cls, buffer_methods, jint(std::size(buffer_methods)));
  env->DeleteLocalRef(cls);
#undef METHOD
}
}  // namespace venture::java
