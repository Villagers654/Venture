package android.graphics;

import java.io.File;

// AOSP graphics/java/android/graphics/HardwareRenderer.java: setupDiskCache
// names the process's persistent EGL blob cache file (libEGL
// egl_set_cache_filename). No HWUI renderer or Skia shader cache exists here.
public class HardwareRenderer {
  private static final String CACHE_PATH_SHADERS = "com.android.opengl.shaders_cache";
  private HardwareRenderer() {}

  /** @hide */
  public static void setupDiskCache(File cacheDir) {
    com.venture.runtime.RuntimeEnvironment.host().setupShadersDiskCache(
        new File(cacheDir, CACHE_PATH_SHADERS).getAbsolutePath());
  }
}
