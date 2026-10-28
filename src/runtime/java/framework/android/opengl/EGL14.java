package android.opengl;

public final class EGL14 {
  public static final int EGL_DEFAULT_DISPLAY = 0, EGL_FALSE = 0, EGL_TRUE = 1,
      EGL_NONE = 0x3038, EGL_SUCCESS = 0x3000, EGL_WIDTH = 0x3057, EGL_HEIGHT = 0x3056,
      EGL_SURFACE_TYPE = 0x3033, EGL_PBUFFER_BIT = 1, EGL_RENDERABLE_TYPE = 0x3040,
      EGL_OPENGL_ES2_BIT = 4, EGL_CONTEXT_CLIENT_VERSION = 0x3098;
  public static final EGLDisplay EGL_NO_DISPLAY = new EGLDisplay(0);
  public static final EGLContext EGL_NO_CONTEXT = new EGLContext(0);
  public static final EGLSurface EGL_NO_SURFACE = new EGLSurface(0);
  private static long handle(EGLObjectHandle object) {
    if (object == null) throw new IllegalArgumentException("null EGL handle");
    return object.getNativeHandle();
  }
  private static void output(int[] array, int offset) {
    if (array == null || offset < 0 || offset >= array.length)
      throw new IllegalArgumentException("EGL output array/offset");
  }
  private static int[] attributes(int[] array, int offset) {
    if (array == null) return null;
    if (offset < 0 || offset >= array.length) throw new IllegalArgumentException("EGL attribute offset");
    for (int i = offset; i < array.length; i += 2)
      if (array[i] == EGL_NONE) return java.util.Arrays.copyOfRange(array, offset, i + 1);
    throw new IllegalArgumentException("EGL attribute list lacks EGL_NONE");
  }
  public static EGLDisplay eglGetDisplay(int displayId) {
    long h = getDisplay(displayId);
    return h == 0 ? EGL_NO_DISPLAY : new EGLDisplay(h);
  }
  public static boolean eglInitialize(EGLDisplay display, int[] major, int majorOffset,
      int[] minor, int minorOffset) {
    if (major != null) output(major, majorOffset);
    if (minor != null) output(minor, minorOffset);
    int[] version = new int[2];
    boolean result = initialize(handle(display), version);
    if (result) {
      if (major != null) major[majorOffset] = version[0];
      if (minor != null) minor[minorOffset] = version[1];
    }
    return result;
  }
  public static boolean eglChooseConfig(EGLDisplay display, int[] attrs, int attrsOffset,
      EGLConfig[] configs, int configsOffset, int configSize, int[] count, int countOffset) {
    output(count, countOffset);
    if (configSize < 0 || (configs != null && (configsOffset < 0
        || configsOffset > configs.length - configSize)))
      throw new IllegalArgumentException("EGL config array/size");
    long[] handles = configs == null ? null : new long[configSize];
    int[] n = new int[1];
    boolean result = chooseConfig(handle(display), attributes(attrs, attrsOffset), handles, n);
    if (result) {
      count[countOffset] = n[0];
      if (configs != null) for (int i = 0; i < Math.min(n[0], configSize); ++i)
        configs[configsOffset + i] = new EGLConfig(handles[i]);
    }
    return result;
  }
  public static EGLSurface eglCreatePbufferSurface(EGLDisplay display, EGLConfig config,
      int[] attrs, int offset) {
    long h = createPbuffer(handle(display), handle(config), attributes(attrs, offset));
    return h == 0 ? EGL_NO_SURFACE : new EGLSurface(h);
  }
  public static EGLContext eglCreateContext(EGLDisplay display, EGLConfig config, EGLContext share,
      int[] attrs, int offset) {
    long h = createContext(handle(display), handle(config), handle(share), attributes(attrs, offset));
    return h == 0 ? EGL_NO_CONTEXT : new EGLContext(h);
  }
  public static boolean eglMakeCurrent(EGLDisplay display, EGLSurface draw, EGLSurface read,
      EGLContext context) { return makeCurrent(handle(display), handle(draw), handle(read), handle(context)); }
  public static boolean eglDestroySurface(EGLDisplay display, EGLSurface surface) {
    return destroySurface(handle(display), handle(surface));
  }
  public static boolean eglDestroyContext(EGLDisplay display, EGLContext context) {
    return destroyContext(handle(display), handle(context));
  }
  public static boolean eglTerminate(EGLDisplay display) { return terminate(handle(display)); }
  public static native int eglGetError();
  private static native long getDisplay(int id);
  private static native boolean initialize(long display, int[] version);
  private static native boolean chooseConfig(long display, int[] attrs, long[] configs, int[] count);
  private static native long createPbuffer(long display, long config, int[] attrs);
  private static native long createContext(long display, long config, long share, int[] attrs);
  private static native boolean makeCurrent(long display, long draw, long read, long context);
  private static native boolean destroySurface(long display, long surface);
  private static native boolean destroyContext(long display, long context);
  private static native boolean terminate(long display);
}
