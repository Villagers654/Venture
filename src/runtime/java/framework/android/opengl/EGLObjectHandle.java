package android.opengl;

// AOSP opengl/java/android/opengl/EGLObjectHandle.java: native EGL handles are
// Java longs. They are host-side values and never exposed as A32 guest pointers.
public abstract class EGLObjectHandle {
  private final long handle;
  protected EGLObjectHandle(long handle) { this.handle = handle; }
  public long getNativeHandle() { return handle; }
  @Override public int hashCode() { return Long.hashCode(handle); }
  @Override public boolean equals(Object other) {
    return other != null && other.getClass() == getClass()
        && ((EGLObjectHandle) other).handle == handle;
  }
}
