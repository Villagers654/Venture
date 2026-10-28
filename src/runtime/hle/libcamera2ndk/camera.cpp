#include "android/errors.h"
#include "hle/libandroid/libandroid.h"
#include "hle/libandroid/parts.h"
#include "android/handles.h"
#include "android/native_window.h"
#include "core/address_space.h"
#include <set>

namespace venture::hle {
namespace {
using namespace android_hle;
// AOSP camera/ndk/include/camera/NdkCameraError.h (these are not Linux errno).
constexpr int invalid = -10001;
struct Manager {};
android::Handles<Manager> managers;
Manager* create() { return managers.create(); }
void destroy(Manager* p) { managers.erase(p); }
// ACameraIdList in NdkCameraManager.h, LP64 padding before cameraIds.
struct IdList {
  std::int32_t count;
  std::uint32_t padding;
  GuestAddr ids;
};
static_assert(sizeof(IdList) == 16 && offsetof(IdList, ids) == 8);
std::mutex lists_mutex;
std::set<GuestAddr> lists;
int list(Manager* p, GuestAddr* out) {
  if (!managers.get(p) || !out) return invalid;
  *out = 0;
  auto address = AddressSpace::get().allocate(sizeof(IdList), kProtRead | kProtWrite, "[camera-id-list]");
  if (!address) return -10003;
  *g2h<IdList>(address) = {};
  {
    std::lock_guard lock(lists_mutex);
    lists.insert(address);
  }
  *out = address;
  return 0;
}
void delete_list(GuestAddr p) {
  std::lock_guard lock(lists_mutex);
  if (lists.erase(p)) AddressSpace::get().free(p);
}
int characteristics(Manager*, const char*, GuestAddr* out) {
  if (out) *out = 0;
  return invalid;
}
int open_camera(Manager*, const char*, void*, GuestAddr* out) {
  if (out) *out = 0;
  log::warn("camera", "ACameraManager_openCamera: no cameras are exposed");
  return invalid;
}
struct Target {
  android::NativeWindow* window;
  explicit Target(android::NativeWindow* w) : window(w) { android::acquire(w); }
  ~Target() { android::release(window); }
};
struct Container {
  std::mutex mutex;
  std::set<GuestAddr> outputs;
};
android::Handles<Target> targets, outputs;
android::Handles<Container> containers;
template <bool Output>
int target_create(android::NativeWindow* window, GuestAddr* out) {
  if (!out || !android::pin_native_window(h2g(window))) return invalid;
  *out = h2g((Output ? outputs : targets).create(window));
  return 0;
}
template <bool Output>
void target_free(Target* p) {
  (Output ? outputs : targets).erase(p);
}
int container_create(GuestAddr* out) {
  if (!out) return invalid;
  *out = h2g(containers.create());
  return 0;
}
void container_free(Container* p) { containers.erase(p); }
template <bool Add>
int container_change(Container* p, Target* output) {
  auto c = containers.get(p);
  if (!c || !outputs.get(output)) return invalid;
  std::lock_guard lock(c->mutex);
  if constexpr (Add)
    c->outputs.insert(h2g(output));
  else
    c->outputs.erase(h2g(output));
  return 0;
}
void fail_object(CallContext& ctx) {
  ctx.return_x(static_cast<std::uint64_t>(static_cast<std::int64_t>(invalid)));
}
void free_absent(CallContext& ctx) {
  if (ctx.x(0)) log::warn("camera", "{}: invalid camera object", ctx.info().name);
}
int create_request(void*, int, GuestAddr* out) {
  if (out) *out = 0;
  return invalid;
}
int create_session(void*, void*, void*, GuestAddr* out) {
  if (out) *out = 0;
  return invalid;
}
}  // namespace
void register_libcamera2ndk() {
  auto library = std::make_shared<HleLibrary>("libcamera2ndk.so");
  auto& lib = *library;
#define API(name, fn) add<&fn>(lib, name)
  API("ACameraManager_create", create);
  API("ACameraManager_delete", destroy);
  API("ACameraManager_getCameraIdList", list);
  API("ACameraManager_deleteCameraIdList", delete_list);
  API("ACameraManager_getCameraCharacteristics", characteristics);
  API("ACameraManager_openCamera", open_camera);
  API("ACameraDevice_createCaptureRequest", create_request);
  API("ACameraDevice_createCaptureSession", create_session);
  API("ACameraOutputTarget_create", target_create<false>);
  API("ACameraOutputTarget_free", target_free<false>);
  API("ACaptureSessionOutput_create", target_create<true>);
  API("ACaptureSessionOutput_free", target_free<true>);
  API("ACaptureSessionOutputContainer_create", container_create);
  API("ACaptureSessionOutputContainer_free", container_free);
  API("ACaptureSessionOutputContainer_add", container_change<true>);
  API("ACaptureSessionOutputContainer_remove", container_change<false>);
#undef API
  for (auto name : {"ACameraDevice_close", "ACameraCaptureSession_setRepeatingRequest",
                    "ACameraCaptureSession_stopRepeating", "ACameraMetadata_getConstEntry",
                    "ACaptureRequest_addTarget", "ACaptureRequest_removeTarget"})
    lib.add_function(name, make_thunk(lib.soname(), name, fail_object));
  for (auto name : {"ACameraCaptureSession_close", "ACameraMetadata_free", "ACaptureRequest_free"})
    lib.add_function(name, make_thunk(lib.soname(), name, free_absent));
  Linker::get().register_hle_library(std::move(library));
}
}  // namespace venture::hle
