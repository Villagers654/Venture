package android.view;

/** Callback/status ABI used by Unity's optional surface snapshot path. */
public final class PixelCopy {
  private PixelCopy() {}
  public static final int SUCCESS = 0, ERROR_UNKNOWN = 1, ERROR_TIMEOUT = 2,
      ERROR_SOURCE_NO_DATA = 3, ERROR_SOURCE_INVALID = 4, ERROR_DESTINATION_INVALID = 5;
  public interface OnPixelCopyFinishedListener {
    void onPixelCopyFinished(int copyResult);
  }
  public static void request(SurfaceView source, android.graphics.Bitmap destination,
      OnPixelCopyFinishedListener listener, android.os.Handler handler) {
    request(source.getHolder().getSurface(), null, destination, listener, handler);
  }
  public static void request(Surface source, android.graphics.Bitmap destination,
      OnPixelCopyFinishedListener listener, android.os.Handler handler) {
    request(source, null, destination, listener, handler);
  }
  public static void request(Surface source, android.graphics.Rect rect, android.graphics.Bitmap destination,
      OnPixelCopyFinishedListener listener, android.os.Handler handler) {
    java.util.Objects.requireNonNull(listener);
    java.util.Objects.requireNonNull(handler);
    if (source == null || !source.isValid()) throw new IllegalArgumentException("Invalid source surface");
    if (destination == null || destination.isRecycled()) throw new IllegalArgumentException("Invalid destination bitmap");
    if (rect != null && rect.isEmpty()) throw new IllegalArgumentException("Empty source rectangle");
    int width = destination.getWidth(), height = destination.getHeight();
    int[] pixels = new int[Math.multiplyExact(width, height)];
    int result = com.venture.runtime.NativeHost.copySurfacePixels(source.ventureNativeWindow(),
        rect == null ? 0 : rect.left, rect == null ? 0 : rect.top,
        rect == null ? 0 : rect.right, rect == null ? 0 : rect.bottom, width, height, pixels);
    if (result == SUCCESS) destination.setPixels(pixels, 0, width, 0, 0, width, height);
    handler.post(() -> listener.onPixelCopyFinished(result));
  }
}
