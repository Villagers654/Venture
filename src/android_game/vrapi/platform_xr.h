// SPDX-License-Identifier: GPL-3.0-or-later
// OpenXR's Android, GLES and Vulkan declarations. Included first by the
// libvrapi.so files that use them: openxr_platform.h declares each part only
// for the platforms defined at its first inclusion (xr/api.h asks for Vulkan).
#pragma once
#ifndef XR_USE_PLATFORM_ANDROID
#define XR_USE_PLATFORM_ANDROID
#endif
#ifndef XR_USE_GRAPHICS_API_OPENGL_ES
#define XR_USE_GRAPHICS_API_OPENGL_ES
#endif
#ifndef XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_VULKAN
#endif
#include <EGL/egl.h>
#include <jni.h>
#include <vulkan/vulkan.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
