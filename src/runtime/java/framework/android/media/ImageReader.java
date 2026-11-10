package android.media;

import android.graphics.ImageFormat;
import android.hardware.HardwareBuffer;
import android.os.Handler;
import android.os.Looper;
import android.view.Surface;
import java.nio.ByteBuffer;
import java.util.concurrent.Executor;

// AOSP media/java/android/media/ImageReader.java (API 19-33) on the NDK image
// reader (hle/libmediandk/image_reader.cpp): a decoder or GL producer renders
// into getSurface(), and each frame becomes an Image with CPU planes or an
// AHardwareBuffer. onImageAvailable runs on the listener's Handler or Executor.
public class ImageReader implements AutoCloseable {
  public interface OnImageAvailableListener {
    void onImageAvailable(ImageReader reader);
  }

  private static native long nativeCreate(int width, int height, int format, long usage, int maxImages);
  private static native void nativeClose(long reader);
  private static native long nativeWindow(long reader);  // an acquired reference
  private static native void nativeListen(long reader);
  private static native int nativeWait(long reader, int timeoutMs);
  // an image, 0 when none is queued, -1 when maxImages are already acquired
  private static native long nativeAcquire(long reader, boolean latest);
  private static native void nativeRelease(long image);
  // width, height, format, planes, timestamp
  private static native long[] nativeInfo(long image);
  // the plane's memory as a direct buffer; strides receives {pixelStride, rowStride}
  private static native ByteBuffer nativePlane(long image, int index, int[] strides);
  private static native long nativeHardwareBuffer(long image);

  private final int width, height, format, maxImages;
  private final long usage;
  private long reader;
  private Surface surface;
  private OnImageAvailableListener listener;
  private Executor executor;
  private Thread notifier;
  private final Object listenerLock = new Object();

  public static ImageReader newInstance(int width, int height, int format, int maxImages) {
    // AOSP: PRIVATE images are GPU-only, others are read by the CPU.
    return new ImageReader(width, height, format, maxImages,
        format == ImageFormat.PRIVATE ? HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE : HardwareBuffer.USAGE_CPU_READ_OFTEN);
  }
  public static ImageReader newInstance(int width, int height, int format, int maxImages, long usage) {
    return new ImageReader(width, height, format, maxImages, usage);
  }
  protected ImageReader(int width, int height, int format, int maxImages, long usage) {
    if (width < 1 || height < 1) throw new IllegalArgumentException("The image dimensions must be positive");
    if (maxImages < 1) throw new IllegalArgumentException("Maximum outstanding image count must be at least 1");
    if (format == ImageFormat.NV21) throw new IllegalArgumentException("NV21 format is not supported");
    this.width = width; this.height = height; this.format = format; this.maxImages = maxImages; this.usage = usage;
    reader = nativeCreate(width, height, format, usage, maxImages);
    if (reader == 0) throw new UnsupportedOperationException("Unsupported ImageReader format 0x" + Integer.toHexString(format));
    surface = Surface.ventureAdopt(nativeWindow(reader));
  }

  public int getWidth() { return width; }
  public int getHeight() { return height; }
  public int getImageFormat() { return format; }
  public int getHardwareBufferFormat() { return format == ImageFormat.PRIVATE ? HardwareBuffer.RGBA_8888 : format; }
  public int getDataSpace() { return 0; }
  public int getMaxImages() { return maxImages; }
  public long getUsage() { return usage; }
  public synchronized Surface getSurface() { return surface; }

  public Image acquireLatestImage() { return acquire(true); }
  public Image acquireNextImage() { return acquire(false); }
  private synchronized Image acquire(boolean latest) {
    if (reader == 0) throw new IllegalStateException("ImageReader is closed");
    long image = nativeAcquire(reader, latest);
    if (image == -1)
      throw new IllegalStateException(String.format(
          "maxImages (%d) has already been acquired, call #close before acquiring more.", maxImages));
    return image == 0 ? null : new SurfaceImage(image);
  }

  public void setOnImageAvailableListener(OnImageAvailableListener listener, Handler handler) {
    Executor target = null;
    if (listener != null) {
      Looper looper = handler != null ? handler.getLooper() : Looper.myLooper();
      if (looper == null)
        throw new IllegalArgumentException("handler is null but the current thread is not a looper");
      Handler h = handler != null ? handler : new Handler(looper);
      target = h::post;
    }
    listen(listener, target);
  }
  public void setOnImageAvailableListenerWithExecutor(OnImageAvailableListener listener, Executor executor) {
    if (executor == null) throw new IllegalArgumentException("executor must not be null");
    listen(listener, executor);
  }
  private void listen(OnImageAvailableListener listener, Executor executor) {
    long id;
    synchronized (this) {
      if (reader == 0) throw new IllegalStateException("ImageReader is closed");
      id = reader;
    }
    synchronized (listenerLock) {
      this.listener = listener;
      this.executor = executor;
      if (listener == null || notifier != null) return;
      nativeListen(id);
      notifier = new Thread(() -> announce(id), "ImageReader-" + width + "x" + height);
      notifier.setDaemon(true);
      notifier.start();
    }
  }
  // One announcement per queued frame, as BufferQueue's onFrameAvailable.
  private void announce(long id) {
    for (;;) {
      int result = isClosed() ? -1 : nativeWait(id, 100);
      OnImageAvailableListener l;
      Executor e;
      synchronized (listenerLock) {
        if (result < 0) { notifier = null; return; }
        if (result == 0) continue;
        l = listener;
        e = executor;
      }
      if (l != null && e != null) e.execute(() -> {
        synchronized (listenerLock) { if (listener != l) return; }
        if (!isClosed()) l.onImageAvailable(this);
      });
    }
  }
  private synchronized boolean isClosed() { return reader == 0; }

  public void discardFreeBuffers() {
    if (isClosed()) throw new IllegalStateException("ImageReader is closed");
  }
  @Override public void close() {
    Thread waiter;
    synchronized (listenerLock) { listener = null; executor = null; waiter = notifier; }
    long id;
    Surface s;
    synchronized (this) {
      if (reader == 0) return;
      id = reader;
      reader = 0;
      s = surface;
      surface = null;
    }
    if (s != null) s.release();
    // The notifier leaves its wait within 100 ms once the reader is closed.
    if (waiter != null && waiter != Thread.currentThread()) {
      try { waiter.join(1000); } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
    }
    nativeClose(id);
  }

  public static final class Builder {
    private final int width, height;
    private int maxImages = 1, format = ImageFormat.UNKNOWN;
    private long usage = HardwareBuffer.USAGE_CPU_READ_OFTEN;
    public Builder(int width, int height) { this.width = width; this.height = height; }
    public Builder setMaxImages(int maxImages) { this.maxImages = maxImages; return this; }
    public Builder setUsage(long usage) { this.usage = usage; return this; }
    public Builder setImageFormat(int format) { this.format = format; return this; }
    public Builder setDefaultHardwareBufferFormat(int format) { this.format = format; return this; }
    public Builder setDefaultDataSpace(int dataSpace) { return this; }
    public ImageReader build() {
      return new ImageReader(width, height, format == ImageFormat.UNKNOWN ? HardwareBuffer.RGBA_8888 : format,
          maxImages, usage);
    }
  }

  private final class SurfaceImage extends Image {
    private long image;
    private final long[] info;
    private Plane[] planes;
    SurfaceImage(long image) {
      this.image = image;
      this.info = nativeInfo(image);
    }
    @Override void throwISEIfImageIsInvalid() {
      if (image == 0) throw new IllegalStateException("Image is already closed");
    }
    @Override public synchronized int getFormat() { throwISEIfImageIsInvalid(); return format; }
    @Override public synchronized int getWidth() { throwISEIfImageIsInvalid(); return (int) info[0]; }
    @Override public synchronized int getHeight() { throwISEIfImageIsInvalid(); return (int) info[1]; }
    @Override public synchronized long getTimestamp() { throwISEIfImageIsInvalid(); return info[4]; }
    @Override public synchronized HardwareBuffer getHardwareBuffer() {
      throwISEIfImageIsInvalid();
      return HardwareBuffer.ventureWrap(nativeHardwareBuffer(image));
    }
    @Override public synchronized Plane[] getPlanes() {
      throwISEIfImageIsInvalid();
      if (planes == null) {
        int count = (int) info[3];
        Plane[] result = new Plane[count];
        for (int i = 0; i < count; ++i) {
          int[] strides = new int[2];
          ByteBuffer buffer = nativePlane(image, i, strides);
          if (buffer == null) throw new UnsupportedOperationException("Image plane " + i + " is not readable");
          result[i] = new SurfacePlane(buffer.order(java.nio.ByteOrder.nativeOrder()), strides[0], strides[1]);
        }
        planes = result;
      }
      return planes.clone();
    }
    @Override public synchronized void close() {
      if (image == 0) return;
      nativeRelease(image);
      image = 0;
      planes = null;
    }
  }
  private static final class SurfacePlane extends Image.Plane {
    private final ByteBuffer buffer;
    private final int pixelStride, rowStride;
    SurfacePlane(ByteBuffer buffer, int pixelStride, int rowStride) {
      this.buffer = buffer; this.pixelStride = pixelStride; this.rowStride = rowStride;
    }
    @Override public int getRowStride() { return rowStride; }
    @Override public int getPixelStride() { return pixelStride; }
    @Override public ByteBuffer getBuffer() { return buffer; }
  }
}
