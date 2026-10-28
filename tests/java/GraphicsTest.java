import android.opengl.*;

/** Exercises UE4's Java GPU probe against the real EGL/GLES provider. */
public final class GraphicsTest {
  private static void check(boolean value, String message) {
    if (!value) throw new AssertionError(message + " EGL error=" + EGL14.eglGetError());
  }
  public static void validate() {
    check(EGL14.eglGetError() == EGL14.EGL_SUCCESS, "initial EGL error");
    int[] version = {-99, -99, -99, -99};
    check(!EGL14.eglInitialize(EGL14.EGL_NO_DISPLAY, version, 1, version, 2), "invalid display rejected");
    check(EGL14.eglGetError() == 0x3008 && EGL14.eglGetError() == EGL14.EGL_SUCCESS,
        "EGL error is reported and reset");
    check(version[0] == -99 && version[1] == -99 && version[2] == -99 && version[3] == -99,
        "failed initialize preserves caller outputs");
    boolean rejected = false;
    try { EGL14.eglInitialize(EGL14.EGL_NO_DISPLAY, version, 4, null, 0); }
    catch (IllegalArgumentException expected) { rejected = true; }
    check(rejected, "native output range rejected");
    rejected = false;
    try { EGL14.eglChooseConfig(EGL14.EGL_NO_DISPLAY, new int[]{EGL14.EGL_NONE}, 0, null, 0, 0, null, 0); }
    catch (IllegalArgumentException expected) { rejected = true; }
    check(rejected, "native required output rejected");
  }
  public static void run() {
    validate();
    int vulkan = com.venture.runtime.RuntimeEnvironment.host().vulkanHardwareVersion();
    // Android hardware features exclude CPU Vulkan devices (e.g. SwiftShader).
    check(vulkan == 0 || vulkan == 0x00400003 || vulkan == 0x00401000,
        "real guest Vulkan capability inventory");
    var packages = new android.content.pm.PackageManager(null);
    check(packages.hasSystemFeature(android.content.pm.PackageManager.FEATURE_VULKAN_HARDWARE_VERSION, vulkan) == (vulkan != 0)
        && packages.hasSystemFeature(android.content.pm.PackageManager.FEATURE_VULKAN_HARDWARE_LEVEL, 0) == (vulkan != 0)
        && !packages.hasSystemFeature(android.content.pm.PackageManager.FEATURE_VULKAN_HARDWARE_LEVEL, 1),
        "PackageManager advertises the native guest capabilities");
    int[] version = {-99, -99, -99, -99};
    EGLDisplay display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
    check(display != EGL14.EGL_NO_DISPLAY, "real display");
    check(EGL14.eglInitialize(display, version, 1, version, 2), "initialize");
    check(version[0] == -99 && version[1] == 1 && version[2] >= 4 && version[3] == -99, "version array offsets");
    EGLConfig[] configs = new EGLConfig[3];
    int[] count = {-99, -99, -99};
    int[] attrs = {-99, EGL14.EGL_SURFACE_TYPE, EGL14.EGL_PBUFFER_BIT,
        EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT, EGL14.EGL_NONE};
    check(EGL14.eglChooseConfig(display, attrs, 1, configs, 1, 1, count, 1), "choose config");
    check(count[0] == -99 && count[1] == 1 && count[2] == -99
        && configs[0] == null && configs[1] != null && configs[2] == null, "config array offsets");
    boolean rejected = false;
    try { EGL14.eglChooseConfig(display, attrs, 1, configs, 3, 1, count, 1); }
    catch (IllegalArgumentException expected) { rejected = true; }
    check(rejected, "config range rejected before native call");
    rejected = false;
    try { EGL14.eglCreatePbufferSurface(display, configs[1], new int[]{EGL14.EGL_WIDTH, 1}, 0); }
    catch (IllegalArgumentException expected) { rejected = true; }
    check(rejected, "unterminated attributes rejected safely");
    EGLSurface surface = EGL14.eglCreatePbufferSurface(display, configs[1],
        new int[]{EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE}, 0);
    check(surface != EGL14.EGL_NO_SURFACE, "pbuffer");
    EGLContext context = EGL14.eglCreateContext(display, configs[1], EGL14.EGL_NO_CONTEXT,
        new int[]{EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
    check(context != EGL14.EGL_NO_CONTEXT, "context");
    check(EGL14.eglMakeCurrent(display, surface, surface, context), "bind context");
    check(GLES20.glGetString(GLES20.GL_VENDOR) != null && GLES20.glGetString(GLES20.GL_RENDERER) != null
        && GLES20.glGetString(GLES20.GL_VERSION).startsWith("OpenGL ES")
        && GLES20.glGetString(GLES20.GL_EXTENSIONS) != null, "actual GLES capabilities");
    check(GLES20.glGetError() == 0, "driver query error");
    check(GLES20.glGetString(0) == null && GLES20.glGetError() == 0x0500
        && GLES20.glGetError() == 0, "real GL error/reset");
    check(EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT), "unbind");
    check(EGL14.eglDestroyContext(display, context), "destroy context");
    EGLContext ue4Context = EGL14.eglCreateContext(display, configs[1], EGL14.EGL_NO_CONTEXT,
        new int[]{EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, 0x30fb, 0, 0x30fc, 0, EGL14.EGL_NONE}, 0);
    check(ue4Context != EGL14.EGL_NO_CONTEXT && EGL14.eglMakeCurrent(display, surface, surface, ue4Context),
        "UE4 KHR context probe");
    check(GLES20.glGetString(GLES20.GL_VERSION).startsWith("OpenGL ES")
        && GLES20.glGetError() == 0, "UE4 KHR context capabilities");
    check(EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT), "unbind UE4 context");
    check(EGL14.eglDestroyContext(display, ue4Context), "destroy UE4 context");
    check(EGL14.eglDestroySurface(display, surface), "destroy surface");
    check(EGL14.eglTerminate(display), "terminate guest display resources");
    check(EGL14.eglInitialize(display, null, 0, null, 0) && EGL14.eglTerminate(display), "reinitialize after probe cleanup");
  }
}
