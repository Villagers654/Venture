// GL textures holding SharedImage pictures (gfx/shared_image.h), for GLES
// consumers such as SurfaceTexture: ANGLE runs on Venture's host device, which
// imports the picture's memory and copies it into the texture's VkImage on the
// GPU. No pixels pass through system memory.
#pragma once

#include <string>

#include "gfx/angle.h"

namespace venture::gfx {
struct SharedImage;

// Whether shared_image_texture can work: ANGLE shares the host device, the
// device imports shared images, and ANGLE exports texture images.
bool gl_imports_shared_images();

struct SharedTexture {
  GLuint texture = 0;                    // immutable RGBA8 storage, the picture's size
  EGLImageKHR image = EGL_NO_IMAGE_KHR;  // the texture's EGLImage sibling
};
// A new texture and its EGLImage holding `picture`, in the current ANGLE
// context; the caller deletes both. GL_TEXTURE_BINDING_2D is preserved.
// Returns false (nothing to delete) when the picture cannot be imported.
bool shared_image_texture(const SharedImage& picture, SharedTexture* out, std::string* error);

}  // namespace venture::gfx
