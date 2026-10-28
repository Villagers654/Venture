#pragma once
#include <cstdint>

#include "gfx/angle.h"
namespace venture::hle { class CallContext; }
namespace venture::gfx {
// Admit an audited forward only when the lifecycle lock is immediately available.
bool try_egl_dispatch(hle::CallContext& ctx);
// Host Java EGL uses the same resource lifecycle and capability filtering as
// guest native EGL. These handles stay on the host side of JNI, even for A32.
struct EglHostApi {
  PFNEGLGETDISPLAYPROC get_display;
  PFNEGLINITIALIZEPROC initialize;
  PFNEGLCHOOSECONFIGPROC choose_config;
  PFNEGLCREATEPBUFFERSURFACEPROC create_pbuffer;
  PFNEGLCREATECONTEXTPROC create_context;
  PFNEGLMAKECURRENTPROC make_current;
  PFNEGLDESTROYSURFACEPROC destroy_surface;
  PFNEGLDESTROYCONTEXTPROC destroy_context;
  PFNEGLTERMINATEPROC terminate;
  PFNEGLGETERRORPROC get_error;
};
const EglHostApi& egl_host_api();
const GLubyte* gles_host_string(GLenum name);
GLenum gles_host_error();
// Context lifecycle hooks keep extension strings and callback userdata alive
// through EGL's deferred destruction of contexts current on other threads.
void gles_context_created(EGLContext context,
                          EGLContext share = EGL_NO_CONTEXT,
                          bool no_error = false);
void gles_context_destroyed(EGLContext context);
void gles_context_bound(EGLContext context);
std::uint64_t egl_frame_count();
}  // namespace venture::gfx
