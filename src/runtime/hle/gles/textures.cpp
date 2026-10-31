#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>

#include "common/log.h"
#include "core/process.h"
#include "gfx/texture_decode.h"
#include "hle/gles/gles_state.h"

namespace venture::gfx {
namespace {
GLint integer(GLenum name) {
  GLint value{};
  angle_function<PFNGLGETINTEGERVPROC, "glGetIntegerv">()(name, &value);
  return value;
}
bool fallback(GLenum format) {
  AstcFormat astc{};
  auto state = current_gl_state();
  return state && state->astc_fallback && astc_format(format, astc);
}
GLenum binding_enum(GLenum target) {
  if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X &&
      target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z)
    return GL_TEXTURE_BINDING_CUBE_MAP;
  switch (target) {
    case GL_TEXTURE_2D:
      return GL_TEXTURE_BINDING_2D;
    case GL_TEXTURE_3D:
      return GL_TEXTURE_BINDING_3D;
    case GL_TEXTURE_CUBE_MAP:
      return GL_TEXTURE_BINDING_CUBE_MAP;
    case GL_TEXTURE_2D_ARRAY:
      return GL_TEXTURE_BINDING_2D_ARRAY;
    case GL_TEXTURE_CUBE_MAP_ARRAY:
      return GL_TEXTURE_BINDING_CUBE_MAP_ARRAY;
    default:
      return 0;
  }
}
GLuint bound_texture(GLenum target) {
  const auto binding = binding_enum(target);
  return binding ? static_cast<GLuint>(integer(binding)) : 0;
}
bool bc7() {
  auto state = current_gl_state();
  return state && state->bc7_fallback;
}
GLenum backing_format(GLenum f) {
  AstcFormat a{};
  astc_format(f, a);
  if (bc7())
    return a.srgb ? GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM_EXT
                  : GL_COMPRESSED_RGBA_BPTC_UNORM_EXT;
  return a.srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
}
std::size_t bc7_size(GLsizei w, GLsizei h, GLsizei depth) {
  return std::size_t((w + 3) / 4) * ((h + 3) / 4) * depth * 16;
}
bool dimensions(GLenum target, GLint level, GLsizei w, GLsizei h,
                GLsizei depth) {
  // KHR_texture_compression_astc_hdr, table 8.19: the LDR profile includes
  // 2D arrays/cubes, but TEXTURE_3D requires HDR or sliced_3d support.
  if (target == GL_TEXTURE_3D) {
    gl_error(GL_INVALID_OPERATION);
    return false;
  }
  if (!binding_enum(target)) {
    gl_error(GL_INVALID_ENUM);
    return false;
  }
  if (level < 0 || level > 31 || w < 0 || h < 0 || depth < 0) {
    gl_error(GL_INVALID_VALUE);
    return false;
  }
  auto limit = integer(target == GL_TEXTURE_2D || target == GL_TEXTURE_2D_ARRAY
                           ? GL_MAX_TEXTURE_SIZE
                           : GL_MAX_CUBE_MAP_TEXTURE_SIZE);
  if (w > std::max(limit >> level, 1) || h > std::max(limit >> level, 1) ||
      (target == GL_TEXTURE_2D_ARRAY &&
       depth > integer(GL_MAX_ARRAY_TEXTURE_LAYERS))) {
    gl_error(GL_INVALID_VALUE);
    return false;
  }
  return true;
}
// Decode PBO bytes only after the buffer's producer has completed: read mapping
// provides GL synchronization. A guest pointer is an offset when a PBO is
// bound. The result is RGBA8, or BC7 blocks when `blocks`.
std::vector<std::uint8_t> decoded(GLenum format, GLsizei w, GLsizei h,
                                  GLsizei depth, GLsizei size,
                                  const void* data, bool blocks = false) {
  GLint buffer = 0;
  if (current_gl_state()->major_version >= 3)
    buffer = integer(GL_PIXEL_UNPACK_BUFFER_BINDING);
  void* mapped = nullptr;
  if (buffer) {
    if (size <= 0) return {};
    mapped = angle_function<PFNGLMAPBUFFERRANGEPROC, "glMapBufferRange">()(
        GL_PIXEL_UNPACK_BUFFER,
        static_cast<GLintptr>(reinterpret_cast<std::uintptr_t>(data)), size,
        GL_MAP_READ_BIT);
    if (!mapped) {
      capture_gl_errors();
      return {};
    }
    data = mapped;
  }
  std::vector<std::uint8_t> output;
  try {
    if (data && w && h && depth) {
      const auto& cache = process_config().cache_dir;
      const std::span input{static_cast<const std::uint8_t*>(data),
                            static_cast<std::size_t>(size)};
      const auto directory =
          cache.empty() ? std::filesystem::path{} : cache / "textures";
      output = blocks ? transcode_astc_bc7(format, w, h, depth, input, directory)
                      : decode_astc(format, w, h, depth, input, directory);
    }
  } catch (const std::bad_alloc&) {
    gl_error(GL_OUT_OF_MEMORY);
  } catch (const std::exception& e) {
    log::error("gles", "ASTC decode failed: {}", e.what());
    gl_error(GL_INVALID_VALUE);
  }
  if (mapped && !angle_function<PFNGLUNMAPBUFFERPROC, "glUnmapBuffer">()(
                    GL_PIXEL_UNPACK_BUFFER)) {
    gl_error(GL_INVALID_OPERATION);
    output.clear();
  }
  return output;
}
struct PackedUpload {
  GLint buffer = 0;
  std::vector<std::pair<GLenum, GLint>> saved;
  PackedUpload() {
    const bool es3 = current_gl_state()->major_version >= 3;
    if (es3) {
      buffer = integer(GL_PIXEL_UNPACK_BUFFER_BINDING);
      angle_function<PFNGLBINDBUFFERPROC, "glBindBuffer">()(
          GL_PIXEL_UNPACK_BUFFER, 0);
    }
    const GLenum names[] = {GL_UNPACK_ALIGNMENT,    GL_UNPACK_ROW_LENGTH,
                            GL_UNPACK_SKIP_ROWS,    GL_UNPACK_SKIP_PIXELS,
                            GL_UNPACK_IMAGE_HEIGHT, GL_UNPACK_SKIP_IMAGES};
    for (unsigned i = 0; i < (es3 ? 6u : 1u); ++i) {
      saved.emplace_back(names[i], integer(names[i]));
      angle_function<PFNGLPIXELSTOREIPROC, "glPixelStorei">()(names[i],
                                                            i == 0 ? 1 : 0);
    }
  }
  ~PackedUpload() {
    for (auto [name, value] : saved)
      angle_function<PFNGLPIXELSTOREIPROC, "glPixelStorei">()(name, value);
    if (current_gl_state()->major_version >= 3)
      angle_function<PFNGLBINDBUFFERPROC, "glBindBuffer">()(
          GL_PIXEL_UNPACK_BUFFER, buffer);
  }
};
void erase_level(GLenum target, GLint level) {
  if (auto state = current_gl_state()) {
    std::lock_guard lock(state->textures->lock);
    state->textures->levels.erase({bound_texture(target), target, level});
    ++state->textures->generation;
  }
}
void compressed_image(GLenum target, GLint level, GLenum format, GLsizei w,
                      GLsizei h, GLsizei depth, GLint border, GLsizei size,
                      const void* data, bool three) {
  if (!materialize_buffer_maps()) return;
  if (!fallback(format)) {
    preserve_gl_errors();
    if (three)
      angle_function<PFNGLCOMPRESSEDTEXIMAGE3DPROC, "glCompressedTexImage3D">()(
          target, level, format, w, h, depth, border, size, data);
    else
      angle_function<PFNGLCOMPRESSEDTEXIMAGE2DPROC, "glCompressedTexImage2D">()(
          target, level, format, w, h, border, size, data);
    if (capture_gl_errors()) erase_level(target, level);
    return;
  }
  preserve_gl_errors();
  if (!dimensions(target, level, w, h, depth)) return;
  if (border || size < 0 ||
      static_cast<std::uint64_t>(size) != astc_size(format, w, h, depth)) {
    gl_error(GL_INVALID_VALUE);
    return;
  }
  if ((three && target != GL_TEXTURE_2D_ARRAY &&
       target != GL_TEXTURE_CUBE_MAP_ARRAY) ||
      (!three &&
       (target == GL_TEXTURE_2D_ARRAY || target == GL_TEXTURE_CUBE_MAP ||
        target == GL_TEXTURE_CUBE_MAP_ARRAY))) {
    gl_error(GL_INVALID_ENUM);
    return;
  }
  const bool blocks = bc7();
  auto pixels = decoded(format, w, h, depth, size, data, blocks);
  const bool pbo = current_gl_state()->major_version >= 3 &&
                   integer(GL_PIXEL_UNPACK_BUFFER_BINDING);
  if (size && (data || pbo) && pixels.empty()) return;
  auto state = current_gl_state();
  std::lock_guard lock(state->textures->lock);
  const auto texture = bound_texture(target);
  PackedUpload packed;
  // Compressed uploads need data; without guest data the level's contents are
  // undefined, so zero blocks serve.
  if (blocks && pixels.empty()) pixels.resize(bc7_size(w, h, depth));
  if (blocks && three)
    angle_function<PFNGLCOMPRESSEDTEXIMAGE3DPROC, "glCompressedTexImage3D">()(
        target, level, backing_format(format), w, h, depth, 0,
        static_cast<GLsizei>(pixels.size()), pixels.data());
  else if (blocks)
    angle_function<PFNGLCOMPRESSEDTEXIMAGE2DPROC, "glCompressedTexImage2D">()(
        target, level, backing_format(format), w, h, 0,
        static_cast<GLsizei>(pixels.size()), pixels.data());
  else if (three)
    angle_function<PFNGLTEXIMAGE3DPROC, "glTexImage3D">()(
        target, level, backing_format(format), w, h, depth, 0, GL_RGBA,
        GL_UNSIGNED_BYTE, pixels.empty() ? nullptr : pixels.data());
  else {
    auto internal = backing_format(format);
    if (state->major_version < 3)
      internal = internal == GL_RGBA8 ? GL_RGBA : GL_SRGB_ALPHA_EXT;
    angle_function<PFNGLTEXIMAGE2DPROC, "glTexImage2D">()(
        target, level, internal, w, h, 0,
        state->major_version < 3 ? internal : GL_RGBA, GL_UNSIGNED_BYTE,
        pixels.empty() ? nullptr : pixels.data());
  }
  if (capture_gl_errors()) {
    state->textures->levels[{texture, target, level}] = {format, w, h, depth};
    ++state->textures->generation;
  }
}
void image2(GLenum t, GLint l, GLenum f, GLsizei w, GLsizei h, GLint b,
            GLsizei s, const void* p) {
  compressed_image(t, l, f, w, h, 1, b, s, p, false);
}
void image3(GLenum t, GLint l, GLenum f, GLsizei w, GLsizei h, GLsizei d,
            GLint b, GLsizei s, const void* p) {
  compressed_image(t, l, f, w, h, d, b, s, p, true);
}
// BC7 regions start on a 4x4 block and end on one or at the level's edge.
// Regions of 5-, 6- and 10-texel ASTC footprints may not: their outer blocks
// are encoded from the region's own edge texels, which can blur up to three
// texels of a neighbouring region. Whole-level uploads are always exact.
void bc7_sub(GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei w,
             GLsizei h, GLsizei depth, GLenum format, GLsizei size,
             const void* data, const TextureLevel& tex, bool three) {
  const bool aligned = x % 4 == 0 && y % 4 == 0 &&
                       (w % 4 == 0 || x + w == tex.width) &&
                       (h % 4 == 0 || y + h == tex.height);
  std::vector<std::uint8_t> blocks;
  GLint bx = x, by = y;
  GLsizei bw = w, bh = h;
  if (aligned) {
    blocks = decoded(format, w, h, depth, size, data, true);
  } else if (auto pixels = decoded(format, w, h, depth, size, data);
             !pixels.empty()) {
    static std::once_flag once;
    std::call_once(once, [] {
      log::warn("gles", "ASTC sub-image off the BC7 block grid: edges of partial "
                        "updates may blur (VENTURE_TEXTURE_COMPRESSION=never keeps RGBA8)");
    });
    bx = x & ~3;
    by = y & ~3;
    bw = std::min<GLsizei>((x + w + 3) & ~3, tex.width) - bx;
    bh = std::min<GLsizei>((y + h + 3) & ~3, tex.height) - by;
    try {
      std::vector<std::uint8_t> region(std::size_t(bw) * bh * depth * 4);
      for (GLsizei layer = 0; layer < depth; ++layer)
        for (GLsizei row = 0; row < bh; ++row)
          for (GLsizei column = 0; column < bw; ++column) {
            const auto sx = std::clamp<GLint>(bx + column - x, 0, w - 1);
            const auto sy = std::clamp<GLint>(by + row - y, 0, h - 1);
            std::memcpy(&region[((std::size_t(layer) * bh + row) * bw + column) * 4],
                        &pixels[((std::size_t(layer) * h + sy) * w + sx) * 4], 4);
          }
      blocks = encode_bc7(bw, bh, depth, region);
    } catch (const std::bad_alloc&) {
      gl_error(GL_OUT_OF_MEMORY);
      return;
    }
  }
  if (blocks.empty()) {
    gl_error(GL_INVALID_VALUE);
    return;
  }
  PackedUpload packed;
  if (three)
    angle_function<PFNGLCOMPRESSEDTEXSUBIMAGE3DPROC, "glCompressedTexSubImage3D">()(
        target, level, bx, by, z, bw, bh, depth, backing_format(format),
        static_cast<GLsizei>(blocks.size()), blocks.data());
  else
    angle_function<PFNGLCOMPRESSEDTEXSUBIMAGE2DPROC, "glCompressedTexSubImage2D">()(
        target, level, bx, by, bw, bh, backing_format(format),
        static_cast<GLsizei>(blocks.size()), blocks.data());
}
void compressed_sub(GLenum target, GLint level, GLint x, GLint y, GLint z,
                    GLsizei w, GLsizei h, GLsizei depth, GLenum format,
                    GLsizei size, const void* data, bool three) {
  if (!materialize_buffer_maps()) return;
  if (!fallback(format)) {
    if (three)
      angle_function<PFNGLCOMPRESSEDTEXSUBIMAGE3DPROC, "glCompressedTexSubImage3D">()(target, level, x, y, z, w, h, depth,
                                       format, size, data);
    else
      angle_function<PFNGLCOMPRESSEDTEXSUBIMAGE2DPROC, "glCompressedTexSubImage2D">()(target, level, x, y, w, h, format, size,
                                       data);
    return;
  }
  preserve_gl_errors();
  if (!dimensions(target, level, w, h, depth)) return;
  auto state = current_gl_state();
  std::lock_guard lock(state->textures->lock);
  auto it =
      state->textures->levels.find({bound_texture(target), target, level});
  if (it == state->textures->levels.end() || it->second.format != format) {
    gl_error(GL_INVALID_OPERATION);
    return;
  }
  const auto& tex = it->second;
  AstcFormat a{};
  astc_format(format, a);
  if (x < 0 || y < 0 || z < 0 || std::int64_t(x) + w > tex.width ||
      std::int64_t(y) + h > tex.height || std::int64_t(z) + depth > tex.depth ||
      size < 0 ||
      static_cast<std::uint64_t>(size) != astc_size(format, w, h, depth)) {
    gl_error(GL_INVALID_VALUE);
    return;
  }
  if (x % a.block_width || y % a.block_height ||
      (w % a.block_width && x + w != tex.width) ||
      (h % a.block_height && y + h != tex.height)) {
    gl_error(GL_INVALID_OPERATION);
    return;
  }
  if (!size) return;
  if (bc7()) {
    bc7_sub(target, level, x, y, z, w, h, depth, format, size, data, tex, three);
    return;
  }
  auto pixels = decoded(format, w, h, depth, size, data);
  if (pixels.empty()) {
    gl_error(GL_INVALID_VALUE);
    return;
  }
  PackedUpload packed;
  if (three)
    angle_function<PFNGLTEXSUBIMAGE3DPROC, "glTexSubImage3D">()(
        target, level, x, y, z, w, h, depth, GL_RGBA, GL_UNSIGNED_BYTE,
        pixels.data());
  else
    angle_function<PFNGLTEXSUBIMAGE2DPROC, "glTexSubImage2D">()(
        target, level, x, y, w, h,
        state->major_version < 3 && a.srgb ? GL_SRGB_ALPHA_EXT : GL_RGBA,
        GL_UNSIGNED_BYTE, pixels.data());
}
void sub2(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f,
          GLsizei s, const void* p) {
  compressed_sub(t, l, x, y, 0, w, h, 1, f, s, p, false);
}
void sub3(GLenum t, GLint l, GLint x, GLint y, GLint z, GLsizei w, GLsizei h,
          GLsizei d, GLenum f, GLsizei s, const void* p) {
  compressed_sub(t, l, x, y, z, w, h, d, f, s, p, true);
}
void storage(GLenum target, GLsizei levels, GLenum format, GLsizei w, GLsizei h,
             GLsizei depth, bool three) {
  const bool replace = fallback(format);
  preserve_gl_errors();
  if (replace && !dimensions(target, 0, w, h, depth)) return;
  if (replace && three && target != GL_TEXTURE_2D_ARRAY &&
      target != GL_TEXTURE_CUBE_MAP_ARRAY) {
    gl_error(GL_INVALID_ENUM);
    return;
  }
  if (three)
    angle_function<PFNGLTEXSTORAGE3DPROC, "glTexStorage3D">()(
        target, levels, replace ? backing_format(format) : format, w, h, depth);
  else
    angle_function<PFNGLTEXSTORAGE2DPROC, "glTexStorage2D">()(
        target, levels, replace ? backing_format(format) : format, w, h);
  if (!capture_gl_errors()) return;
  auto state = current_gl_state();
  if (!state) return;
  std::lock_guard lock(state->textures->lock);
  auto texture = bound_texture(target);
  ++state->textures->generation;
  std::erase_if(state->textures->levels, [&](const auto& pair) {
    return std::get<0>(pair.first) == texture;
  });
  if (!replace) return;
  for (GLint level = 0; level < levels; ++level) {
    if (target == GL_TEXTURE_CUBE_MAP)
      for (GLenum face = GL_TEXTURE_CUBE_MAP_POSITIVE_X;
           face <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z; ++face)
        state->textures->levels[{texture, face, level}] = {format, w, h, 1};
    else
      state->textures->levels[{texture, target, level}] = {format, w, h, depth};
    w = std::max(w / 2, 1);
    h = std::max(h / 2, 1);
  }
}
void storage2(GLenum t, GLsizei n, GLenum f, GLsizei w, GLsizei h) {
  storage(t, n, f, w, h, 1, false);
}
void storage3(GLenum t, GLsizei n, GLenum f, GLsizei w, GLsizei h, GLsizei d) {
  storage(t, n, f, w, h, d, true);
}
template <class T>
bool level_query(GLenum target, GLint level, GLenum name, T* value) {
  auto state = current_gl_state();
  if (!state || !value) return false;
  std::lock_guard lock(state->textures->lock);
  auto it =
      state->textures->levels.find({bound_texture(target), target, level});
  if (it == state->textures->levels.end()) return false;
  if (name == GL_TEXTURE_INTERNAL_FORMAT)
    *value = static_cast<T>(it->second.format);
  else if (name == GL_TEXTURE_COMPRESSED)
    *value = GL_TRUE;
  else
    return false;
  return true;
}
void level_int(GLenum t, GLint l, GLenum name, GLint* value) {
  if (!level_query(t, l, name, value))
    angle_function<PFNGLGETTEXLEVELPARAMETERIVPROC, "glGetTexLevelParameteriv">()(
        t, l, name, value);
}
void level_float(GLenum t, GLint l, GLenum name, GLfloat* value) {
  if (!level_query(t, l, name, value))
    angle_function<PFNGLGETTEXLEVELPARAMETERFVPROC, "glGetTexLevelParameterfv">()(
        t, l, name, value);
}
void delete_textures(GLsizei n, const GLuint* ids) {
  if (n >= 0 && ids)
    if (auto state = current_gl_state()) {
      std::lock_guard lock(state->textures->lock);
      ++state->textures->generation;
      for (GLsizei i = 0; i < n; ++i)
        std::erase_if(state->textures->levels, [&](const auto& p) {
          return std::get<0>(p.first) == ids[i];
        });
    }
  delete_buffer_textures(n, ids);
}
void tex_image2(GLenum t, GLint l, GLint f, GLsizei w, GLsizei h, GLint b,
                GLenum format, GLenum type, const void* p) {
  if (!materialize_buffer_maps()) return;
  preserve_gl_errors();
  angle_function<PFNGLTEXIMAGE2DPROC, "glTexImage2D">()(t, l, f, w, h, b, format,
                                                      type, p);
  if (capture_gl_errors()) erase_level(t, l);
}
void tex_image3(GLenum t, GLint l, GLint f, GLsizei w, GLsizei h, GLsizei d,
                GLint b, GLenum format, GLenum type, const void* p) {
  if (!materialize_buffer_maps()) return;
  preserve_gl_errors();
  angle_function<PFNGLTEXIMAGE3DPROC, "glTexImage3D">()(t, l, f, w, h, d, b,
                                                      format, type, p);
  if (capture_gl_errors()) erase_level(t, l);
}
bool emulated_level(GLenum target, GLint level) {
  auto state = current_gl_state();
  if (!state) return false;
  std::lock_guard lock(state->textures->lock);
  return state->textures->levels.contains(
      {bound_texture(target), target, level});
}
void tex_sub2(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h,
              GLenum f, GLenum type, const void* p) {
  if (!materialize_buffer_maps()) return;
  if (emulated_level(t, l)) {
    gl_error(GL_INVALID_OPERATION);
    return;
  }
  angle_function<PFNGLTEXSUBIMAGE2DPROC, "glTexSubImage2D">()(t, l, x, y, w, h, f,
                                                            type, p);
}
void tex_sub3(GLenum t, GLint l, GLint x, GLint y, GLint z, GLsizei w,
              GLsizei h, GLsizei d, GLenum f, GLenum type, const void* p) {
  if (!materialize_buffer_maps()) return;
  if (emulated_level(t, l)) {
    gl_error(GL_INVALID_OPERATION);
    return;
  }
  angle_function<PFNGLTEXSUBIMAGE3DPROC, "glTexSubImage3D">()(t, l, x, y, z, w, h,
                                                            d, f, type, p);
}
void copy_sub2(GLenum t, GLint l, GLint x, GLint y, GLint sx, GLint sy,
               GLsizei w, GLsizei h) {
  if (emulated_level(t, l)) {
    gl_error(GL_INVALID_OPERATION);
    return;
  }
  angle_function<PFNGLCOPYTEXSUBIMAGE2DPROC, "glCopyTexSubImage2D">()(
      t, l, x, y, sx, sy, w, h);
}
void copy_sub3(GLenum t, GLint l, GLint x, GLint y, GLint z, GLint sx, GLint sy,
               GLsizei w, GLsizei h) {
  if (emulated_level(t, l)) {
    gl_error(GL_INVALID_OPERATION);
    return;
  }
  angle_function<PFNGLCOPYTEXSUBIMAGE3DPROC, "glCopyTexSubImage3D">()(
      t, l, x, y, z, sx, sy, w, h);
}
void copy_image2(GLenum t, GLint l, GLenum internal, GLint x, GLint y,
                 GLsizei w, GLsizei h, GLint border) {
  preserve_gl_errors();
  angle_function<PFNGLCOPYTEXIMAGE2DPROC, "glCopyTexImage2D">()(t, l, internal, x,
                                                              y, w, h, border);
  if (capture_gl_errors()) erase_level(t, l);
}
bool framebuffer_astc(GLenum target) {
  auto state = current_gl_state();
  if (!state || !state->astc_fallback) return false;
  const GLenum binding = target == GL_READ_FRAMEBUFFER
                             ? GL_READ_FRAMEBUFFER_BINDING
                             : GL_DRAW_FRAMEBUFFER_BINDING;
  const auto framebuffer = static_cast<GLuint>(
      integer(state->major_version < 3 ? GL_FRAMEBUFFER_BINDING : binding));
  if (framebuffer == 0) return false;
  std::lock_guard lock(state->textures->lock);
  if (state->textures->levels.empty()) return false;
  if (state->framebuffer_generation != state->textures->generation) {
    state->framebuffer_cache.clear();
    state->framebuffer_generation = state->textures->generation;
  }
  if (auto it = state->framebuffer_cache.find(framebuffer);
      it != state->framebuffer_cache.end())
    return it->second;
  auto query = angle_function<PFNGLGETFRAMEBUFFERATTACHMENTPARAMETERIVPROC, "glGetFramebufferAttachmentParameteriv">();
  auto count = state->major_version < 3 ? 1 : integer(GL_MAX_COLOR_ATTACHMENTS);
  for (GLint i = 0; i < count; ++i) {
    GLint type{}, object{}, level{};
    query(target, GL_COLOR_ATTACHMENT0 + i,
          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
    if (type != GL_TEXTURE) continue;
    query(target, GL_COLOR_ATTACHMENT0 + i,
          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &object);
    query(target, GL_COLOR_ATTACHMENT0 + i,
          GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL, &level);
    for (const auto& [key, value] : state->textures->levels)
      if (std::get<0>(key) == static_cast<GLuint>(object) &&
          std::get<2>(key) == level) {
        state->framebuffer_cache[framebuffer] = true;
        return true;
      }
  }
  state->framebuffer_cache[framebuffer] = false;
  return false;
}
GLenum framebuffer_status(GLenum target) {
  auto result = angle_function<PFNGLCHECKFRAMEBUFFERSTATUSPROC, "glCheckFramebufferStatus">()(target);
  if (result == GL_FRAMEBUFFER_COMPLETE && framebuffer_astc(target))
    return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
  return result;
}
void internal_format(GLenum target, GLenum format, GLenum pname, GLsizei size,
                     GLint* values) {
  if (fallback(format) && target == GL_RENDERBUFFER &&
      (pname == GL_NUM_SAMPLE_COUNTS || pname == GL_SAMPLES)) {
    if (size < 0) {
      gl_error(GL_INVALID_VALUE);
      return;
    }
    if (pname == GL_NUM_SAMPLE_COUNTS && size && values) *values = 0;
    return;
  }
  angle_function<PFNGLGETINTERNALFORMATIVPROC, "glGetInternalformativ">()(
      target, format, pname, size, values);
}
void generate_mipmap(GLenum target) {
  auto state = current_gl_state();
  if (state) {
    std::lock_guard lock(state->textures->lock);
    auto texture = bound_texture(target);
    for (const auto& [key, value] : state->textures->levels)
      if (std::get<0>(key) == texture) {
        // GLES 3.0 GenerateMipmap errors: compressed base images are
        // ineligible.
        gl_error(GL_INVALID_OPERATION);
        return;
      }
  }
  angle_function<PFNGLGENERATEMIPMAPPROC, "glGenerateMipmap">()(target);
}
}  // namespace
bool try_gl_draw_dispatch(hle::CallContext& ctx) {
  auto* state = current_gl_dispatch_state();
  std::unique_lock<std::recursive_mutex> buffers_lock, textures_lock;
  if (state && state->buffers32) {
    buffers_lock = std::unique_lock(state->buffers32->lock, std::try_to_lock);
    if (!buffers_lock.owns_lock() ||
        state->buffers32->emulated_maps.load(std::memory_order_relaxed)) return false;
  }
  if (state && state->astc_fallback) {
    textures_lock = std::unique_lock(state->textures->lock, std::try_to_lock);
    if (!textures_lock.owns_lock()) return false;
  }
  // Keep both guards through validation and forwarding: a shared context must
  // not introduce an emulated map between admission and materialization. ASTC
  // checks retain their framebuffer cache and share-group generation invalidation.
  ctx.info().handler(ctx);
  return true;
}
bool validate_gl_call(GlCheck check) {
  if (check != GlCheck::framebuffer_change && !materialize_buffer_maps()) return false;
  if (check == GlCheck::framebuffer_change) {
    if (auto state = current_gl_state()) state->framebuffer_cache.clear();
    return true;
  }
  const bool draw = check == GlCheck::draw, read = check == GlCheck::read,
             blit = check == GlCheck::blit;
  if (!draw && !read && !blit) return true;
  auto state = current_gl_state();
  if (!state || !state->astc_fallback) {
    if (draw || blit) begin_gl_render_timing();
    return true;
  }
  if (((draw || blit) &&
       framebuffer_astc(state->major_version < 3 ? GL_FRAMEBUFFER
                                                 : GL_DRAW_FRAMEBUFFER)) ||
      ((read || blit) &&
       framebuffer_astc(state->major_version < 3 ? GL_FRAMEBUFFER
                                                 : GL_READ_FRAMEBUFFER))) {
    gl_error(GL_INVALID_FRAMEBUFFER_OPERATION);
    return false;
  }
  if (draw || blit) begin_gl_render_timing();
  return true;
}
std::vector<GLint> guest_compressed_formats() {
  GLint count = integer(GL_NUM_COMPRESSED_TEXTURE_FORMATS);
  std::vector<GLint> result(static_cast<std::size_t>(std::max(count, 0)));
  if (count)
    angle_function<PFNGLGETINTEGERVPROC, "glGetIntegerv">()(
        GL_COMPRESSED_TEXTURE_FORMATS, result.data());
  if (current_gl_state() && current_gl_state()->astc_fallback)
    for (GLint base : {0x93B0, 0x93D0})
      for (GLint i = 0; i < 14; ++i)
        if (std::ranges::find(result, base + i) == result.end())
          result.push_back(base + i);
  return result;
}
hle::Handler texture_gl_handler(std::string_view name) {
  if (name == "glCompressedTexImage2D") return wrap<&image2>();
  if (name == "glCompressedTexImage3D" || name == "glCompressedTexImage3DOES")
    return wrap<&image3>();
  if (name == "glCompressedTexSubImage2D") return wrap<&sub2>();
  if (name == "glCompressedTexSubImage3D" ||
      name == "glCompressedTexSubImage3DOES")
    return wrap<&sub3>();
  if (name == "glTexStorage2D" || name == "glTexStorage2DEXT")
    return wrap<&storage2>();
  if (name == "glTexStorage3D" || name == "glTexStorage3DEXT")
    return wrap<&storage3>();
  if (name == "glGetTexLevelParameteriv") return wrap<&level_int>();
  if (name == "glGetTexLevelParameterfv") return wrap<&level_float>();
  if (name == "glDeleteTextures") return wrap<&delete_textures>();
  if (name == "glTexImage2D") return wrap<&tex_image2>();
  if (name == "glTexImage3D" || name == "glTexImage3DOES")
    return wrap<&tex_image3>();
  if (name == "glTexSubImage2D") return wrap<&tex_sub2>();
  if (name == "glTexSubImage3D" || name == "glTexSubImage3DOES")
    return wrap<&tex_sub3>();
  if (name == "glCopyTexSubImage2D") return wrap<&copy_sub2>();
  if (name == "glCopyTexImage2D") return wrap<&copy_image2>();
  if (name == "glCopyTexSubImage3D" || name == "glCopyTexSubImage3DOES")
    return wrap<&copy_sub3>();
  if (name == "glCheckFramebufferStatus") return wrap<&framebuffer_status>();
  if (name == "glGetInternalformativ") return wrap<&internal_format>();
  if (name == "glGenerateMipmap") return wrap<&generate_mipmap>();
  return nullptr;
}
}  // namespace venture::gfx
