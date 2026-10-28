#include "gfx/a32_handles.h"
#include <map>

namespace venture::gfx::a32 {
namespace {
template<Handle Kind> struct Object { void* host; };
Token32Registry registry;
std::mutex mutex;
std::map<std::pair<Handle, void*>, std::uint32_t> tokens;
template<class F> decltype(auto) visit(Handle kind, F&& f) {
  switch (kind) {
    case Handle::display: return f.template operator()<Object<Handle::display>>();
    case Handle::config: return f.template operator()<Object<Handle::config>>();
    case Handle::context: return f.template operator()<Object<Handle::context>>();
    case Handle::surface: return f.template operator()<Object<Handle::surface>>();
    case Handle::image: return f.template operator()<Object<Handle::image>>();
    case Handle::egl_sync: return f.template operator()<Object<Handle::egl_sync>>();
    case Handle::gl_sync: return f.template operator()<Object<Handle::gl_sync>>();
  }
  throw std::logic_error("unknown graphics handle type");
}
}
Handle handle_kind(std::string_view tag) {
  if (tag.starts_with("EGLDisplay")) return Handle::display;
  if (tag.starts_with("EGLConfig")) return Handle::config;
  if (tag.starts_with("EGLContext")) return Handle::context;
  if (tag.starts_with("EGLSurface")) return Handle::surface;
  if (tag.starts_with("EGLSync")) return Handle::egl_sync;
  if (tag.starts_with("EGLImage") || tag == "GLeglImageOES") return Handle::image;
  if (tag == "GLsync") return Handle::gl_sync;
  throw std::logic_error("unsupported graphics opaque type");
}
std::uint32_t encode(Handle kind, void* host) {
  if (!host) return 0;
  std::lock_guard lock(mutex);
  auto key = std::pair(kind, host);
  if (auto it = tokens.find(key); it != tokens.end()) return it->second;
  auto token = visit(kind, [&]<class T> { return registry.insert(std::make_shared<T>(T{host})).value; });
  tokens.emplace(key, token);
  return token;
}
void* decode(Handle kind, std::uint32_t token) {
  if (!token) return nullptr;
  return visit(kind, [&]<class T>() -> void* {
    auto object = registry.resolve(Token32<T>{token});
    if (!object) throw BadHandle{kind};
    return object->host;
  });
}
void retire(Handle kind, void* host) {
  std::lock_guard lock(mutex);
  auto it = tokens.find({kind, host});
  if (it == tokens.end()) return;
  visit(kind, [&]<class T> { registry.erase(Token32<T>{it->second}); });
  tokens.erase(it);
}
void clear_egl_handles() {
  std::lock_guard lock(mutex);
  for (auto it = tokens.begin(); it != tokens.end();) {
    // EGLDisplay identities survive eglTerminate and can be initialized again.
    if (it->first.first == Handle::display || it->first.first == Handle::gl_sync) { ++it; continue; }
    visit(it->first.first, [&]<class T> { registry.erase(Token32<T>{it->second}); });
    it = tokens.erase(it);
  }
}
} // namespace venture::gfx::a32
