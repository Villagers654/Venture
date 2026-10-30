#include "hle/egl/egl.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "android/native_window.h"
#include "common/log.h"
#include "core/signals.h"
#include "core/thread.h"
#include "core/guest_storage.h"
#include "core/process.h"
#include "core/profile.h"
#include "gfx/dispatch.h"
#include "gfx/a32_handles.h"
#include "hle/gles/gles_state.h"

namespace venture::gfx {
namespace {
thread_local EGLint last_error = EGL_SUCCESS;
std::recursive_mutex state_lock;
bool initialized = false;
struct Surface {
  android::NativeWindow* window = nullptr;
  EGLint render_buffer = EGL_BACK_BUFFER;
  ~Surface() {
    if (window) android::release(window);
  }
};
std::set<EGLConfig> configs;
std::set<EGLContext> contexts;
std::set<EGLContext> no_error_contexts;
std::map<EGLSurface, std::shared_ptr<Surface>> surfaces;
std::set<EGLSyncKHR> syncs;
std::set<EGLImageKHR> images;
thread_local std::shared_ptr<Surface> current_draw, current_read;
std::atomic<std::uint64_t> frames{0};
std::string extension_string;

void fail(EGLint error, const char* operation) {
  last_error = error;
  log::debug("egl", "{} failed: EGL 0x{:x}", operation, error);
}
template <class T, class... A>
auto host(const char* name, A... args) {
  auto fn = angle_function<T>(name);
  using R = decltype(fn(args...));
  if (!fn) {
    fail(EGL_BAD_ACCESS, name);
    return R{};
  }
  auto result = fn(args...);
  auto error = angle_function<PFNEGLGETERRORPROC>("eglGetError")();
  if (error != EGL_SUCCESS) fail(error, name);
  return result;
}
bool valid_display(EGLDisplay d, bool require_init = true) {
  if (!d || d != angle_display()) {
    fail(EGL_BAD_DISPLAY, "display");
    return false;
  }
  if (require_init && !initialized) {
    fail(EGL_NOT_INITIALIZED, "display");
    return false;
  }
  return true;
}
bool valid_config(EGLConfig c) {
  if (configs.contains(c)) return true;
  fail(EGL_BAD_CONFIG, "config");
  return false;
}
bool valid_context(EGLContext c) {
  if (contexts.contains(c)) return true;
  fail(EGL_BAD_CONTEXT, "context");
  return false;
}
bool valid_surface(EGLSurface s) {
  if (surfaces.contains(s)) return true;
  fail(EGL_BAD_SURFACE, "surface");
  return false;
}
bool valid_sync(EGLSyncKHR s) {
  if (syncs.contains(s)) return true;
  fail(EGL_BAD_PARAMETER, "sync");
  return false;
}
struct ContextRequest {
  bool no_error = false, debug_or_robust = false;
};
bool attributes(const EGLint* input, std::vector<EGLint>& out,
                bool choose = false, ContextRequest* context = nullptr) {
  if (input) {
    for (std::size_t i = 0; i < 256; i += 2) {
      EGLint key = input[i];
      if (key == EGL_NONE) {
        out.push_back(EGL_NONE);
        return true;
      }
      EGLint value = input[i + 1];
      if (context) {
        // EGL_KHR_create_context_no_error. ANGLE always validates (a no-error
        // ANGLE context turns invalid guest calls into host crashes); gles.cpp
        // gives the guest the KHR_no_error contract instead.
        if (key == EGL_CONTEXT_OPENGL_NO_ERROR_KHR) {
          if (value != EGL_TRUE && value != EGL_FALSE) {
            fail(EGL_BAD_ATTRIBUTE, "no-error context");
            return false;
          }
          context->no_error = value == EGL_TRUE;
          continue;
        }
        if ((key == EGL_CONTEXT_FLAGS_KHR &&
             (value & (EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR |
                       EGL_CONTEXT_OPENGL_ROBUST_ACCESS_BIT_KHR))) ||
            ((key == EGL_CONTEXT_OPENGL_DEBUG ||
              key == EGL_CONTEXT_OPENGL_ROBUST_ACCESS ||
              key == EGL_CONTEXT_OPENGL_ROBUST_ACCESS_EXT) &&
             value == EGL_TRUE))
          context->debug_or_robust = true;
      }
      if (key == EGL_RECORDABLE_ANDROID ||
          key == 0x314C /* EGL_FRONT_BUFFER_AUTO_REFRESH_ANDROID */ ||
          key == EGL_PROTECTED_CONTENT_EXT) {
        fail(EGL_BAD_ATTRIBUTE, "unavailable Android surface capability");
        return false;
      }
      if (context && key == EGL_CONTEXT_PRIORITY_LEVEL_IMG) {
        if (value != EGL_CONTEXT_PRIORITY_HIGH_IMG &&
            value != EGL_CONTEXT_PRIORITY_MEDIUM_IMG &&
            value != EGL_CONTEXT_PRIORITY_LOW_IMG) {
          fail(EGL_BAD_ATTRIBUTE, "context priority");
          return false;
        }
        // EGL_IMG_context_priority permits the implementation to substitute
        // MEDIUM. Keep ANGLE on its one shared medium-priority queue.
        continue;
      }
      if (choose && key == EGL_SURFACE_TYPE && value != EGL_DONT_CARE &&
          (value & EGL_WINDOW_BIT))
        value = (value & ~EGL_WINDOW_BIT) | EGL_PBUFFER_BIT;
      out.push_back(key);
      out.push_back(value);
    }
    fail(EGL_BAD_ATTRIBUTE, "unterminated attributes");
    return false;
  }
  out.push_back(EGL_NONE);
  return true;
}
std::vector<EGLint> narrow_attributes(const std::int64_t* input, bool& ok) {
  std::vector<EGLint> result;
  ok = false;
  if (input)
    for (std::size_t i = 0; i < 256; i += 2) {
      if (input[i] == EGL_NONE) {
        ok = true;
        break;
      }
      if (input[i] < INT32_MIN || input[i] > INT32_MAX ||
          input[i + 1] < INT32_MIN || input[i + 1] > INT32_MAX)
        break;
      result.push_back(static_cast<EGLint>(input[i]));
      result.push_back(static_cast<EGLint>(input[i + 1]));
    }
  else
    ok = true;
  if (!ok) fail(EGL_BAD_ATTRIBUTE, "64-bit attributes");
  result.push_back(EGL_NONE);
  return result;
}
void build_extensions(EGLDisplay d) {
  const char* raw =
      host<PFNEGLQUERYSTRINGPROC>("eglQueryString", d, EGL_EXTENSIONS);
  extension_string.clear();
  constexpr const char* allowed[] = {"EGL_KHR_create_context",
                                     "EGL_KHR_surfaceless_context",
                                     "EGL_KHR_gl_colorspace",
                                     "EGL_KHR_fence_sync",
                                     "EGL_KHR_wait_sync",
                                     "EGL_KHR_image_base",
                                     "EGL_KHR_gl_texture_2D_image",
                                     "EGL_KHR_get_all_proc_addresses",
                                     "EGL_KHR_create_context_no_error",
                                     "EGL_EXT_create_context_robustness"};
  for (auto name : allowed)
    if (raw && has_extension(raw, name)) {
      if (!extension_string.empty()) extension_string += ' ';
      extension_string += name;
    }
  extension_string += " EGL_IMG_context_priority";
}

#define VENTURE_EGL_DECLARE
#include "egl_dispatch.inc"
#undef VENTURE_EGL_DECLARE

EGLDisplay guest_eglGetDisplay(EGLNativeDisplayType native) {
  if (native != EGL_DEFAULT_DISPLAY) {
    fail(EGL_BAD_PARAMETER, "eglGetDisplay");
    return EGL_NO_DISPLAY;
  }
  auto display = angle_display();
  if (!display) fail(EGL_NOT_INITIALIZED, "eglGetDisplay");
  return display;
}
EGLBoolean guest_eglInitialize(EGLDisplay d, EGLint* major, EGLint* minor) {
  std::lock_guard lock(state_lock);
  if (!valid_display(d, false)) return EGL_FALSE;
  if (!initialized) {
    EGLint count{};
    if (!host<PFNEGLGETCONFIGSPROC>("eglGetConfigs", d, nullptr, 0, &count))
      return EGL_FALSE;
    std::vector<EGLConfig> all(static_cast<std::size_t>(count));
    if (!host<PFNEGLGETCONFIGSPROC>("eglGetConfigs", d, all.data(), count,
                                    &count))
      return EGL_FALSE;
    configs.insert(all.begin(), all.begin() + count);
    build_extensions(d);
    initialized = true;
  }
  if (major) *major = 1;
  if (minor) *minor = 5;
  return EGL_TRUE;
}
EGLint guest_eglGetError() {
  const auto e = last_error;
  last_error = EGL_SUCCESS;
  return e;
}
EGLBoolean guest_eglTerminate(EGLDisplay d) {
  std::lock_guard lock(state_lock);
  if (!valid_display(d, false)) return EGL_FALSE;
  // The backing display/device is process-owned and shared with XR. Destroy
  // guest resources individually; ANGLE supplies deferred current destruction.
  for (auto c : contexts) {
    host<PFNEGLDESTROYCONTEXTPROC>("eglDestroyContext", d, c);
    gles_context_destroyed(c);
  }
  for (const auto& [s, state] : surfaces)
    host<PFNEGLDESTROYSURFACEPROC>("eglDestroySurface", d, s);
  for (auto s : syncs)
    host<PFNEGLDESTROYSYNCKHRPROC>("eglDestroySyncKHR", d, s);
  for (auto i : images)
    host<PFNEGLDESTROYIMAGEKHRPROC>("eglDestroyImageKHR", d, i);
  contexts.clear();
  no_error_contexts.clear();
  surfaces.clear();
  syncs.clear();
  images.clear();
  configs.clear();
  initialized = false;
  if (process_config().isa() == GuestIsa::a32) a32::clear_egl_handles();
  return EGL_TRUE;
}
EGLBoolean guest_eglChooseConfig(EGLDisplay d, const EGLint* a,
                                 EGLConfig* output, EGLint capacity,
                                 EGLint* count) {
  std::lock_guard lock(state_lock);
  if (!valid_display(d)) return EGL_FALSE;
  std::vector<EGLint> translated;
  if (!attributes(a, translated, true)) return EGL_FALSE;
  return host<PFNEGLCHOOSECONFIGPROC>("eglChooseConfig", d, translated.data(),
                                      output, capacity, count);
}
EGLBoolean guest_eglGetConfigs(EGLDisplay d, EGLConfig* out, EGLint capacity,
                               EGLint* count) {
  std::lock_guard lock(state_lock);
  if (!valid_display(d)) return EGL_FALSE;
  return host<PFNEGLGETCONFIGSPROC>("eglGetConfigs", d, out, capacity, count);
}
EGLBoolean guest_eglGetConfigAttrib(EGLDisplay d, EGLConfig c, EGLint attr,
                                    EGLint* value) {
  std::lock_guard lock(state_lock);
  if (!valid_display(d) || !valid_config(c)) return EGL_FALSE;
  auto ok =
      host<PFNEGLGETCONFIGATTRIBPROC>("eglGetConfigAttrib", d, c, attr, value);
  if (ok && attr == EGL_SURFACE_TYPE && (*value & EGL_PBUFFER_BIT))
    *value |= EGL_WINDOW_BIT;
  return ok;
}
EGLContext guest_eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext share,
                                  const EGLint* a) {
  std::lock_guard lock(state_lock);
  if (!valid_display(d) || !valid_config(c) || (share && !valid_context(share)))
    return EGL_NO_CONTEXT;
  std::vector<EGLint> translated;
  ContextRequest request;
  if (!attributes(a, translated, false, &request)) return EGL_NO_CONTEXT;
  // EGL_KHR_create_context_no_error: BAD_MATCH for a no-error debug or
  // robustness context, or a share context with a different no-error value.
  if ((request.no_error && request.debug_or_robust) ||
      (share && no_error_contexts.contains(share) != request.no_error)) {
    fail(EGL_BAD_MATCH, "eglCreateContext no-error");
    return EGL_NO_CONTEXT;
  }
  auto result = host<PFNEGLCREATECONTEXTPROC>("eglCreateContext", d, c, share,
                                              translated.data());
  if (result) {
    contexts.insert(result);
    if (request.no_error) no_error_contexts.insert(result);
    gles_context_created(result, share, request.no_error);
    profile::mark("egl_context");
  } else {
    // The request, the config's renderable APIs and the bound API: enough to
    // tell which one the host rejected (EGL_BAD_MATCH names none of them).
    std::string requested;
    for (const EGLint* p = translated.data(); p && *p != EGL_NONE; p += 2)
      requested += std::format("{}{:#x}={:#x}", requested.empty() ? "" : " ", p[0], p[1]);
    const EGLint error = last_error;
    EGLint renderable = 0, config_id = 0;
    host<PFNEGLGETCONFIGATTRIBPROC>("eglGetConfigAttrib", d, c, EGL_RENDERABLE_TYPE, &renderable);
    host<PFNEGLGETCONFIGATTRIBPROC>("eglGetConfigAttrib", d, c, EGL_CONFIG_ID, &config_id);
    const EGLenum api = host<PFNEGLQUERYAPIPROC>("eglQueryAPI");
    last_error = error;  // the guest's eglGetError still reports the failure
    log::warn("egl", "eglCreateContext failed: error {:#x}; attributes [{}]; config {} renderable {:#x}; API {:#x}",
              error, requested, config_id, renderable, api);
  }
  return result;
}
EGLBoolean guest_eglDestroyContext(EGLDisplay d, EGLContext c) {
  std::lock_guard lock(state_lock);
  if (!valid_display(d) || !valid_context(c)) return EGL_FALSE;
  auto ok = host<PFNEGLDESTROYCONTEXTPROC>("eglDestroyContext", d, c);
  if (ok) {
    contexts.erase(c);
    no_error_contexts.erase(c);
    gles_context_destroyed(c);
    if (process_config().isa() == GuestIsa::a32) a32::retire(a32::Handle::context, c);
  }
  return ok;
}
EGLSurface guest_eglCreatePbufferSurface(EGLDisplay d, EGLConfig c,
                                         const EGLint* a) {
  std::lock_guard lock(state_lock);
  if (!valid_display(d) || !valid_config(c)) return EGL_NO_SURFACE;
  auto result =
      host<PFNEGLCREATEPBUFFERSURFACEPROC>("eglCreatePbufferSurface", d, c, a);
  if (result) surfaces.emplace(result, std::make_shared<Surface>());
  return result;
}
EGLSurface guest_eglCreateWindowSurface(EGLDisplay d, EGLConfig c,
                                        GuestAddr window, const EGLint* a) {
  std::lock_guard lock(state_lock);
  if (!valid_display(d) || !valid_config(c)) return EGL_NO_SURFACE;
  auto* native = android::native_window_from_guest(window);
  if (!native) {
    fail(EGL_BAD_NATIVE_WINDOW, "eglCreateWindowSurface");
    return EGL_NO_SURFACE;
  }
  for (const auto& [s, state] : surfaces)
    if (state->window == native) {
      fail(EGL_BAD_ALLOC, "window already has EGL surface");
      return EGL_NO_SURFACE;
    }
  std::vector<EGLint> requested, translated;
  if (!attributes(a, requested)) return EGL_NO_SURFACE;
  EGLint render_buffer = EGL_BACK_BUFFER;
  // Window attributes cannot be blindly reinterpreted as pbuffer attributes.
  for (std::size_t i = 0; i + 1 < requested.size(); i += 2) {
    if (requested[i] == EGL_RENDER_BUFFER) {
      if (requested[i + 1] != EGL_BACK_BUFFER &&
          requested[i + 1] != EGL_SINGLE_BUFFER) {
        fail(EGL_BAD_ATTRIBUTE, "window render buffer");
        return EGL_NO_SURFACE;
      }
      // EGL 1.5 sections 3.5.1/3.5.6: retain the requested buffer for
      // eglQuerySurface. GLES may use a back buffer even for SINGLE_BUFFER;
      // eglQueryContext reports ANGLE's actual pbuffer rendering buffer.
      render_buffer = requested[i + 1];
    } else if (requested[i] == EGL_GL_COLORSPACE_KHR) {
      translated.push_back(requested[i]);
      translated.push_back(requested[i + 1]);
    } else {
      fail(EGL_BAD_ATTRIBUTE, "window attributes");
      return EGL_NO_SURFACE;
    }
  }
  EGLint width{}, height{};
  {
    std::lock_guard native_lock(native->lock);
    width = native->width;
    height = native->height;
  }
  if (width <= 0 || height <= 0) {
    fail(EGL_BAD_NATIVE_WINDOW, "window dimensions");
    return EGL_NO_SURFACE;
  }
  translated.insert(translated.end(),
                    {EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE});
  auto s = host<PFNEGLCREATEPBUFFERSURFACEPROC>("eglCreatePbufferSurface", d, c,
                                                translated.data());
  if (s) {
    auto state = std::make_shared<Surface>();
    android::acquire(native);
    state->window = native;
    state->render_buffer = render_buffer;
    surfaces.emplace(s, std::move(state));
  }
  return s;
}
EGLBoolean guest_eglDestroySurface(EGLDisplay d, EGLSurface s) {
  std::lock_guard lock(state_lock);
  if (!valid_display(d) || !valid_surface(s)) return EGL_FALSE;
  auto ok = host<PFNEGLDESTROYSURFACEPROC>("eglDestroySurface", d, s);
  if (ok) {
    surfaces.erase(s);
    if (process_config().isa() == GuestIsa::a32) a32::retire(a32::Handle::surface, s);
  }
  return ok;
}
EGLBoolean guest_eglMakeCurrent(EGLDisplay d, EGLSurface draw, EGLSurface read,
                                EGLContext c) {
  std::lock_guard lock(state_lock);
  if (!valid_display(d) || (c && !valid_context(c)) ||
      (draw && !valid_surface(draw)) || (read && !valid_surface(read)))
    return EGL_FALSE;
  auto ok = host<PFNEGLMAKECURRENTPROC>("eglMakeCurrent", d, draw, read, c);
  if (ok) {
    current_draw = draw ? surfaces.at(draw) : nullptr;
    current_read = read ? surfaces.at(read) : nullptr;
    gles_context_bound(c);
  }
  return ok;
}
EGLContext guest_eglGetCurrentContext() {
  return host<PFNEGLGETCURRENTCONTEXTPROC>("eglGetCurrentContext");
}
EGLDisplay guest_eglGetCurrentDisplay() {
  return host<PFNEGLGETCURRENTDISPLAYPROC>("eglGetCurrentDisplay");
}
EGLSurface guest_eglGetCurrentSurface(EGLint which) {
  return host<PFNEGLGETCURRENTSURFACEPROC>("eglGetCurrentSurface", which);
}
EGLBoolean guest_eglSwapBuffers(EGLDisplay d, EGLSurface s) {
  std::lock_guard lock(state_lock);
  if (!valid_display(d) || !valid_surface(s)) return EGL_FALSE;
  auto* window = surfaces.at(s)->window;
  if (window && android::has_window_image_consumer(window)) {
    // The pbuffer is this window's GPU producer, not a separate presentation
    // sink. Snapshot its completed default framebuffer into the same window
    // frame used by ANativeWindow_unlockAndPost and Vulkan WSI.
    auto context = guest_eglGetCurrentContext();
    auto draw = guest_eglGetCurrentSurface(EGL_DRAW);
    auto read = guest_eglGetCurrentSurface(EGL_READ);
    if (!context || draw != s) { fail(EGL_BAD_ACCESS, "Surface producer context"); return EGL_FALSE; }
    EGLint width{}, height{}, version{};
    host<PFNEGLQUERYSURFACEPROC>("eglQuerySurface", d, s, EGL_WIDTH, &width);
    host<PFNEGLQUERYSURFACEPROC>("eglQuerySurface", d, s, EGL_HEIGHT, &height);
    host<PFNEGLQUERYCONTEXTPROC>("eglQueryContext", d, context, EGL_CONTEXT_CLIENT_VERSION, &version);
    if (width <= 0 || height <= 0 || std::uint64_t(width) * height > INT32_MAX / 4) {
      fail(EGL_BAD_ALLOC, "Surface producer dimensions"); return EGL_FALSE;
    }
    std::vector<std::uint8_t> rgba;
    try { rgba.resize(std::size_t(width) * height * 4); }
    catch (const std::bad_alloc&) { fail(EGL_BAD_ALLOC, "Surface producer allocation"); return EGL_FALSE; }
    if (read != s && !host<PFNEGLMAKECURRENTPROC>("eglMakeCurrent", d, s, s, context)) return EGL_FALSE;
    auto get = angle_function<PFNGLGETINTEGERVPROC>("glGetIntegerv");
    auto bind = angle_function<PFNGLBINDFRAMEBUFFERPROC>("glBindFramebuffer");
    auto pixel_store = angle_function<PFNGLPIXELSTOREIPROC>("glPixelStorei");
    preserve_gl_errors();
    GLint framebuffer{}, alignment{}, pbo{}, row_length{}, skip_rows{}, skip_pixels{}, default_read_buffer{};
    GLenum target = version >= 3 ? GL_READ_FRAMEBUFFER : GL_FRAMEBUFFER;
    get(version >= 3 ? GL_READ_FRAMEBUFFER_BINDING : GL_FRAMEBUFFER_BINDING, &framebuffer);
    get(GL_PACK_ALIGNMENT, &alignment);
    if (version >= 3) {
      get(GL_PIXEL_PACK_BUFFER_BINDING, &pbo);
      get(GL_PACK_ROW_LENGTH, &row_length); get(GL_PACK_SKIP_ROWS, &skip_rows); get(GL_PACK_SKIP_PIXELS, &skip_pixels);
      angle_function<PFNGLBINDBUFFERPROC>("glBindBuffer")(GL_PIXEL_PACK_BUFFER, 0);
      pixel_store(GL_PACK_ROW_LENGTH, 0); pixel_store(GL_PACK_SKIP_ROWS, 0); pixel_store(GL_PACK_SKIP_PIXELS, 0);
    }
    bind(target, 0);
    if (version >= 3) {
      get(GL_READ_BUFFER, &default_read_buffer);
      angle_function<PFNGLREADBUFFERPROC>("glReadBuffer")(GL_BACK);
    }
    pixel_store(GL_PACK_ALIGNMENT, 1);
    angle_function<PFNGLREADPIXELSPROC>("glReadPixels")(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    auto error = angle_function<PFNGLGETERRORPROC>("glGetError")();
    pixel_store(GL_PACK_ALIGNMENT, alignment);
    if (version >= 3) angle_function<PFNGLREADBUFFERPROC>("glReadBuffer")(default_read_buffer);
    bind(target, framebuffer);
    if (version >= 3) {
      angle_function<PFNGLBINDBUFFERPROC>("glBindBuffer")(GL_PIXEL_PACK_BUFFER, pbo);
      pixel_store(GL_PACK_ROW_LENGTH, row_length); pixel_store(GL_PACK_SKIP_ROWS, skip_rows); pixel_store(GL_PACK_SKIP_PIXELS, skip_pixels);
    }
    if (read != s && !host<PFNEGLMAKECURRENTPROC>("eglMakeCurrent", d, draw, read, context)) return EGL_FALSE;
    if (error != GL_NO_ERROR || android::publish_window_rgba(window, rgba, width, height, true)) {
      log::error("egl", "Surface producer readback failed: GL=0x{:x}", error);
      fail(EGL_BAD_ALLOC, "Surface producer publication"); return EGL_FALSE;
    }
  }
  auto ok = host<PFNEGLSWAPBUFFERSPROC>("eglSwapBuffers", d, s);
  if (ok) ++frames;
  return ok;
}
EGLBoolean guest_eglSwapInterval(EGLDisplay d, EGLint interval) {
  std::lock_guard lock(state_lock);
  if (!valid_display(d)) return EGL_FALSE;
  return host<PFNEGLSWAPINTERVALPROC>("eglSwapInterval", d, interval);
}
EGLBoolean guest_eglQuerySurface(EGLDisplay d, EGLSurface s, EGLint attr,
                                 EGLint* value) {
  std::lock_guard lock(state_lock);
  if (!valid_display(d) || !valid_surface(s)) return EGL_FALSE;
  const auto& surface = surfaces.at(s);
  if (attr == EGL_RENDER_BUFFER && surface->window && value) {
    *value = surface->render_buffer;
    return EGL_TRUE;
  }
  return host<PFNEGLQUERYSURFACEPROC>("eglQuerySurface", d, s, attr, value);
}
EGLBoolean guest_eglQueryContext(EGLDisplay d, EGLContext c, EGLint attr,
                                 EGLint* value) {
  std::lock_guard lock(state_lock);
  if (!valid_display(d) || !valid_context(c)) return EGL_FALSE;
  if (attr == EGL_CONTEXT_PRIORITY_LEVEL_IMG && value) {
    *value = EGL_CONTEXT_PRIORITY_MEDIUM_IMG;
    return EGL_TRUE;
  }
  return host<PFNEGLQUERYCONTEXTPROC>("eglQueryContext", d, c, attr, value);
}
const char* guest_eglQueryString(EGLDisplay d, EGLint name) {
  std::lock_guard lock(state_lock);
  if (!d && name == EGL_EXTENSIONS) return "EGL_EXT_client_extensions";
  if (!valid_display(d)) return nullptr;
  if (name == EGL_EXTENSIONS) return extension_string.c_str();
  if (name == EGL_VERSION) return "1.5 Venture (ANGLE Vulkan)";
  return host<PFNEGLQUERYSTRINGPROC>("eglQueryString", d, name);
}
const char* query_string32(EGLDisplay d, EGLint name) {
  auto* text = guest_eglQueryString(d, name);
  return text ? low_string(text) : nullptr;
}
GuestAddr guest_eglGetProcAddress(const char* name) {
  return name ? guest_proc_address(name) : 0;
}
EGLBoolean guest_eglBindAPI(EGLenum api) {
  if (api != EGL_OPENGL_ES_API) {
    fail(EGL_BAD_PARAMETER, "eglBindAPI");
    return EGL_FALSE;
  }
  return host<PFNEGLBINDAPIPROC>("eglBindAPI", api);
}
EGLenum guest_eglQueryAPI() { return host<PFNEGLQUERYAPIPROC>("eglQueryAPI"); }
EGLBoolean guest_eglReleaseThread() {
  auto ok = host<PFNEGLRELEASETHREADPROC>("eglReleaseThread");
  if (ok) {
    current_draw.reset();
    current_read.reset();
    gles_context_bound(EGL_NO_CONTEXT);
  }
  return ok;
}
EGLBoolean guest_eglWaitClient() {
  return host<PFNEGLWAITCLIENTPROC>("eglWaitClient");
}
EGLBoolean guest_eglWaitGL() { return host<PFNEGLWAITGLPROC>("eglWaitGL"); }
EGLBoolean guest_eglWaitNative(EGLint engine) {
  return host<PFNEGLWAITNATIVEPROC>("eglWaitNative", engine);
}
EGLBoolean guest_eglSurfaceAttrib(EGLDisplay d, EGLSurface s, EGLint attr,
                                  EGLint value) {
  std::lock_guard lock(state_lock);
  if (!valid_display(d) || !valid_surface(s)) return EGL_FALSE;
  return host<PFNEGLSURFACEATTRIBPROC>("eglSurfaceAttrib", d, s, attr, value);
}
EGLBoolean guest_eglBindTexImage(EGLDisplay d, EGLSurface s, EGLint buffer) {
  std::lock_guard lock(state_lock);
  if (!valid_display(d) || !valid_surface(s)) return EGL_FALSE;
  return host<PFNEGLBINDTEXIMAGEPROC>("eglBindTexImage", d, s, buffer);
}
EGLBoolean guest_eglReleaseTexImage(EGLDisplay d, EGLSurface s, EGLint buffer) {
  std::lock_guard lock(state_lock);
  if (!valid_display(d) || !valid_surface(s)) return EGL_FALSE;
  return host<PFNEGLRELEASETEXIMAGEPROC>("eglReleaseTexImage", d, s, buffer);
}
EGLSyncKHR guest_eglCreateSyncKHR(EGLDisplay d, EGLenum type, const EGLint* a) {
  std::lock_guard lock(state_lock);
  if (!valid_display(d)) return EGL_NO_SYNC_KHR;
  if (type != EGL_SYNC_FENCE_KHR) {
    fail(EGL_BAD_ATTRIBUTE, "eglCreateSyncKHR type");
    return EGL_NO_SYNC_KHR;
  }
  auto result = host<PFNEGLCREATESYNCKHRPROC>("eglCreateSyncKHR", d, type, a);
  if (result) syncs.insert(result);
  return result;
}
EGLBoolean guest_eglDestroySyncKHR(EGLDisplay d, EGLSyncKHR s) {
  std::lock_guard lock(state_lock);
  if (!valid_display(d) || !valid_sync(s)) return EGL_FALSE;
  auto ok = host<PFNEGLDESTROYSYNCKHRPROC>("eglDestroySyncKHR", d, s);
  if (ok) {
    syncs.erase(s);
    if (process_config().isa() == GuestIsa::a32) a32::retire(a32::Handle::egl_sync, s);
  }
  return ok;
}
EGLint guest_eglClientWaitSyncKHR(EGLDisplay d, EGLSyncKHR s, EGLint flags,
                                  EGLTimeKHR timeout) {
  {
    std::lock_guard lock(state_lock);
    if (!valid_display(d) || !valid_sync(s)) return EGL_FALSE;
  }
  // Bound driver waits so the calling guest thread can deliver GC/signal work.
  const auto start = std::chrono::steady_clock::now();
  auto remaining = timeout;
  for (;;) {
    auto result = host<PFNEGLCLIENTWAITSYNCKHRPROC>(
        "eglClientWaitSyncKHR", d, s, flags,
        std::min<EGLTimeKHR>(remaining, 1000000));
    if (result != EGL_TIMEOUT_EXPIRED_KHR || remaining == 0) return result;
    flags = 0;
    if (auto* thread = GuestThread::current())
      signals::deliver_pending(*thread);
    if (timeout != EGL_FOREVER_KHR) {
      const auto elapsed = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - start)
              .count());
      if (elapsed >= timeout) return EGL_TIMEOUT_EXPIRED_KHR;
      remaining = timeout - elapsed;
    }
  }
}
EGLBoolean guest_eglGetSyncAttribKHR(EGLDisplay d, EGLSyncKHR s, EGLint attr,
                                     EGLint* value) {
  std::lock_guard lock(state_lock);
  if (!valid_display(d) || !valid_sync(s)) return EGL_FALSE;
  return host<PFNEGLGETSYNCATTRIBKHRPROC>("eglGetSyncAttribKHR", d, s, attr,
                                          value);
}
EGLint guest_eglWaitSyncKHR(EGLDisplay d, EGLSyncKHR s, EGLint flags) {
  std::lock_guard lock(state_lock);
  if (!valid_display(d) || !valid_sync(s)) return EGL_FALSE;
  return host<PFNEGLWAITSYNCKHRPROC>("eglWaitSyncKHR", d, s, flags);
}
EGLImageKHR guest_eglCreateImageKHR(EGLDisplay d, EGLContext c, EGLenum target,
                                    EGLClientBuffer buffer, const EGLint* a) {
  std::lock_guard lock(state_lock);
  if (!valid_display(d)) return EGL_NO_IMAGE_KHR;
  // Android native buffers are not host EGLClientBuffers. Until AHardwareBuffer
  // integration exists, reject them and hide EGL_ANDROID_image_native_buffer.
  if (target != EGL_GL_TEXTURE_2D_KHR) {
    fail(EGL_BAD_PARAMETER, "eglCreateImageKHR target");
    return EGL_NO_IMAGE_KHR;
  }
  if (!valid_context(c)) return EGL_NO_IMAGE_KHR;
  auto result = host<PFNEGLCREATEIMAGEKHRPROC>("eglCreateImageKHR", d, c,
                                               target, buffer, a);
  if (result) images.insert(result);
  return result;
}
EGLBoolean guest_eglDestroyImageKHR(EGLDisplay d, EGLImageKHR image) {
  std::lock_guard lock(state_lock);
  if (!valid_display(d)) return EGL_FALSE;
  if (!images.contains(image)) {
    fail(EGL_BAD_PARAMETER, "image");
    return EGL_FALSE;
  }
  auto ok = host<PFNEGLDESTROYIMAGEKHRPROC>("eglDestroyImageKHR", d, image);
  if (ok) {
    images.erase(image);
    if (process_config().isa() == GuestIsa::a32) a32::retire(a32::Handle::image, image);
  }
  return ok;
}
EGLSync guest_eglCreateSync(EGLDisplay d, EGLenum t, const std::int64_t* a) {
  bool ok;
  auto narrow = narrow_attributes(a, ok);
  return ok ? guest_eglCreateSyncKHR(d, t, narrow.data()) : EGL_NO_SYNC;
}
EGLBoolean guest_eglDestroySync(EGLDisplay d, EGLSync s) {
  return guest_eglDestroySyncKHR(d, s);
}
EGLint guest_eglClientWaitSync(EGLDisplay d, EGLSync s, EGLint flags,
                               EGLTime time) {
  return guest_eglClientWaitSyncKHR(d, s, flags, time);
}
EGLBoolean guest_eglGetSyncAttrib(EGLDisplay d, EGLSync s, EGLint attr,
                                  std::int64_t* value) {
  if (!value) {
    fail(EGL_BAD_PARAMETER, "eglGetSyncAttrib");
    return EGL_FALSE;
  }
  EGLint narrow{};
  auto ok = guest_eglGetSyncAttribKHR(d, s, attr, &narrow);
  if (ok) *value = narrow;
  return ok;
}
EGLBoolean guest_eglWaitSync(EGLDisplay d, EGLSync s, EGLint flags) {
  return guest_eglWaitSyncKHR(d, s, flags);
}
EGLImage guest_eglCreateImage(EGLDisplay d, EGLContext c, EGLenum target,
                              EGLClientBuffer b, const std::int64_t* a) {
  bool ok;
  auto narrow = narrow_attributes(a, ok);
  return ok ? guest_eglCreateImageKHR(d, c, target, b, narrow.data())
            : EGL_NO_IMAGE;
}
EGLBoolean guest_eglDestroyImage(EGLDisplay d, EGLImage i) {
  return guest_eglDestroyImageKHR(d, i);
}

// EGL/egl.h ILP32 probes: EGLConfig[] and EGLAttrib[] have four-byte
// elements. Never let ANGLE write its eight-byte handles into guest arrays.
EGLBoolean configs32(EGLDisplay d, const EGLint* attrs, std::uint32_t* out,
                     EGLint capacity, EGLint* count, bool choose) {
  if (capacity < 0 || !count) { fail(EGL_BAD_PARAMETER, "EGLConfig array"); return EGL_FALSE; }
  std::vector<EGLConfig> host_configs(out ? static_cast<std::size_t>(capacity) : 0);
  EGLint written = 0;
  auto ok = choose ? guest_eglChooseConfig(d, attrs, out ? host_configs.data() : nullptr, capacity, &written)
                   : guest_eglGetConfigs(d, out ? host_configs.data() : nullptr, capacity, &written);
  if (ok) {
    if (out) for (EGLint i = 0; i < std::min(written, capacity); ++i) {
      auto token = a32::encode(a32::Handle::config, host_configs[i]);
      hle::a32::write_bytes(h2g(out) + 4ull * i, &token, 4);
    }
    hle::a32::write_bytes(h2g(count), &written, 4);
  }
  return ok;
}
EGLBoolean choose_config32(EGLDisplay d, const EGLint* attrs, std::uint32_t* out, EGLint capacity, EGLint* count) {
  return configs32(d, attrs, out, capacity, count, true);
}
EGLBoolean get_configs32(EGLDisplay d, std::uint32_t* out, EGLint capacity, EGLint* count) {
  return configs32(d, nullptr, out, capacity, count, false);
}
std::vector<std::int64_t> attribs32(const std::int32_t* attrs) {
  std::vector<std::int64_t> result;
  if (attrs) for (unsigned i = 0; i < 256; ++i) {
    std::int32_t word;
    hle::a32::read_bytes(h2g(attrs) + 4ull * i, &word, 4);
    result.push_back(word);
    if (!(i & 1) && word == EGL_NONE) return result;
  }
  if (!attrs) result.push_back(EGL_NONE);
  else { fail(EGL_BAD_ATTRIBUTE, "EGLAttrib array"); result.clear(); }
  return result;
}
EGLSync create_sync32(EGLDisplay d, EGLenum type, const std::int32_t* attrs) {
  auto a = attribs32(attrs);
  return a.empty() ? EGL_NO_SYNC : guest_eglCreateSync(d, type, a.data());
}
EGLImage create_image32(EGLDisplay d, EGLContext c, EGLenum target, EGLClientBuffer b, const std::int32_t* attrs) {
  auto a = attribs32(attrs);
  return a.empty() ? EGL_NO_IMAGE : guest_eglCreateImage(d, c, target, b, a.data());
}
EGLBoolean get_sync_attrib32(EGLDisplay d, EGLSync s, EGLint attr, std::int32_t* value) {
  if (!value) { fail(EGL_BAD_PARAMETER, "eglGetSyncAttrib"); return EGL_FALSE; }
  std::int64_t wide = 0;
  auto ok = guest_eglGetSyncAttrib(d, s, attr, &wide);
  if (ok) {
    auto word = hle::a32::narrow_out({hle::a32::Kind::OutLong}, wide);
    hle::a32::write_bytes(h2g(value), &word, 4);
  }
  return ok;
}
template<auto F> hle::Handler egl_adapter32() {
  return [](hle::CallContext& ctx) {
    // Keep token decode, the semantic operation and token publication atomic
    // with resource destruction. Client waits retain their interruptible,
    // unlocked driver-wait path; concurrent destruction while waiting is the
    // same application lifetime constraint as on native EGL.
    std::unique_lock lock(state_lock, std::defer_lock);
    if (ctx.info().name != "eglClientWaitSync" && ctx.info().name != "eglClientWaitSyncKHR") lock.lock();
    a32::invoke(F, ctx);
  };
}
}  // namespace
namespace a32 {
void invalid_handle(hle::CallContext& ctx, Handle kind) {
  if (ctx.info().library == "libEGL.so") {
    auto error = kind == Handle::display ? EGL_BAD_DISPLAY : kind == Handle::config ? EGL_BAD_CONFIG :
                 kind == Handle::context ? EGL_BAD_CONTEXT : kind == Handle::surface ? EGL_BAD_SURFACE : EGL_BAD_PARAMETER;
    fail(error, ctx.info().name.c_str());
  } else if (ctx.info().name != "glIsSync") gl_error(GL_INVALID_VALUE);
  ctx.return_x(ctx.info().name == "glClientWaitSync" ? GL_WAIT_FAILED : 0);
}
void abi_failure(hle::CallContext& ctx, int error) {
  hle::a32::fail(ctx, error);
  if (ctx.info().library == "libEGL.so") fail(EGL_BAD_PARAMETER, ctx.info().name.c_str());
  else gl_error(GL_INVALID_OPERATION);
  // EGLBoolean and GLboolean errors are false; never return the generic
  // libc -1 (which would appear to be success for a boolean/pointer result).
  ctx.return_x(ctx.info().name == "glClientWaitSync" ? GL_WAIT_FAILED : 0);
}
}
void register_egl(HleLibrary& library) {
  if (process_config().isa() == GuestIsa::a32) {
#define VENTURE_EGL_ADAPT(name, fn) hle::a32::register_adapter("libEGL.so", name, egl_adapter32<&fn>());
#include "egl_a32_adapters.inc"
#undef VENTURE_EGL_ADAPT
  }
#include "egl_dispatch.inc"
}
std::uint64_t egl_frame_count() { return frames.load(); }
bool try_egl_dispatch(hle::CallContext& ctx) {
  // A lifecycle owner may release GuestStorage and await InvalidationGate.
  // Never wait for that owner while this callback still holds the gate.
  std::unique_lock lock(state_lock, std::try_to_lock);
  if (!lock.owns_lock()) return false;
  ctx.info().handler(ctx); // Existing recursive locking and error capture stay intact.
  return true;
}
const EglHostApi& egl_host_api() {
  static const EglHostApi api{guest_eglGetDisplay, guest_eglInitialize,
    guest_eglChooseConfig, guest_eglCreatePbufferSurface, guest_eglCreateContext,
    guest_eglMakeCurrent, guest_eglDestroySurface, guest_eglDestroyContext,
    guest_eglTerminate, guest_eglGetError};
  return api;
}
}  // namespace venture::gfx
