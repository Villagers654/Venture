package android.view;
public class Surface implements AutoCloseable {
  public static final int ROTATION_0 = 0, ROTATION_90 = 1, ROTATION_180 = 2, ROTATION_270 = 3;
  private long nativeHandle;
  // JNI ANativeWindow_toSurface adopts an already acquired native reference.
  private Surface(long ownedHandle) {
    if (ownedHandle == 0) throw new IllegalArgumentException("Null native window");
    nativeHandle = ownedHandle;
  }
  public Surface(android.graphics.SurfaceTexture texture) {
    this(java.util.Objects.requireNonNull(texture).ventureAcquireWindow());
  }
  public Surface() {
    this(1920, 1080);
  }
  public Surface(int width, int height) {
    this(width, height, 1);
  }
  Surface(int width, int height, int format) {
    nativeHandle =
        com.venture.runtime.RuntimeEnvironment.host().createSurface(width, height, format);
    if (nativeHandle == 0)
      throw new IllegalStateException("Host surface creation failed");
  }
  public static final int FRAME_RATE_COMPATIBILITY_DEFAULT = 0, FRAME_RATE_COMPATIBILITY_FIXED_SOURCE = 1;
  public static final int CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS = 0, CHANGE_FRAME_RATE_ALWAYS = 1;
  public void setFrameRate(float frameRate, int compatibility) {
    setFrameRate(frameRate, compatibility, CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS);
  }
  // AOSP Surface.setFrameRate: a hint to the compositor, validated like
  // Surface::setFrameRate. The XR runtime owns the display rate here.
  public void setFrameRate(float frameRate, int compatibility, int changeFrameRateStrategy) {
    if (!(frameRate >= 0) || Float.isInfinite(frameRate)
        || (compatibility != FRAME_RATE_COMPATIBILITY_DEFAULT && compatibility != FRAME_RATE_COMPATIBILITY_FIXED_SOURCE)
        || (changeFrameRateStrategy != CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS && changeFrameRateStrategy != CHANGE_FRAME_RATE_ALWAYS))
      throw new IllegalArgumentException("Invalid argument to Surface.setFrameRate()");
    if (!isValid()) throw new IllegalStateException("Surface has been released.");
  }
  public synchronized long ventureNativeWindow() {
    return nativeHandle;
  }
  public synchronized boolean isValid() {
    return nativeHandle != 0;
  }
  public synchronized void release() {
    if (nativeHandle != 0) {
      com.venture.runtime.RuntimeEnvironment.host().releaseSurface(nativeHandle);
      nativeHandle = 0;
    }
  }
  public void close() {
    release();
  }
}
