#pragma once
#define EGL_NO_PLATFORM_SPECIFIC_TYPES
// Extension declarations depend on the core GL typedefs. Keep this order.
// clang-format off
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
// clang-format on

#include <filesystem>
#include <string>
#include <string_view>

namespace venture::gfx {
// Set before first use; defaults to VENTURE_ANGLE_DIR, then the application
// directory.
bool configure_angle(const std::filesystem::path& directory);
bool initialize_angle();
EGLDisplay angle_display();
void* angle_proc(const char* name);
template <class T>
T angle_function(const char* name) {
  return reinterpret_cast<T>(angle_proc(name));
}
// ANGLE modules stay loaded for the process. A literal entry name therefore
// identifies one immutable result, including a missing optional procedure.
namespace detail {
template <std::size_t N> struct AngleName {
  char text[N];
  constexpr AngleName(const char (&name)[N]) {
    for (std::size_t i = 0; i < N; ++i) text[i] = name[i];
  }
};
template <AngleName Name, auto Resolve = angle_proc>
void* stable_angle_proc() {
  static void* const proc = Resolve(Name.text);
  return proc;
}
}
template <class T, detail::AngleName Name>
T angle_function() {
  return reinterpret_cast<T>(detail::stable_angle_proc<Name>());
}
bool has_extension(std::string_view list, std::string_view name);
const std::string& angle_error();
bool angle_shared_device();
bool angle_uses_general_layout();
// Host-only release on the owning thread, after its final guest call unwinds.
// Does not initialize ANGLE or re-enter guest callbacks.
bool release_thread_for_os_quit() noexcept;
bool terminate_for_os_quit();
// External queue users must hold GpuDevice::queue_lock THEN this lock. Never
// hold ANGLE's nonrecursive lock while calling GL entry points.
bool lock_angle_queue();
void unlock_angle_queue();
}  // namespace venture::gfx
