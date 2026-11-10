package android.hardware;

// AOSP core/java/android/hardware/HardwareBuffer.java (API 26-33): a Java
// reference to an AHardwareBuffer. Each object holds one reference, released
// by close(); AHardwareBuffer_fromHardwareBuffer and _toHardwareBuffer cross
// between the two.
public final class HardwareBuffer implements AutoCloseable {
  public static final int RGBA_8888 = 1, RGBX_8888 = 2, RGB_888 = 3, RGB_565 = 4, RGBA_FP16 = 0x16,
      RGBA_1010102 = 0x2b, BLOB = 0x21, YCBCR_420_888 = 0x23, D_16 = 0x30, D_24 = 0x31, DS_24UI8 = 0x32,
      D_FP32 = 0x33, DS_FP32UI8 = 0x34, S_UI8 = 0x35, YCBCR_P010 = 0x36, R_8 = 0x38;
  public static final long USAGE_CPU_READ_RARELY = 2, USAGE_CPU_READ_OFTEN = 3, USAGE_CPU_WRITE_RARELY = 0x20,
      USAGE_CPU_WRITE_OFTEN = 0x30, USAGE_GPU_SAMPLED_IMAGE = 0x100, USAGE_GPU_COLOR_OUTPUT = 0x200,
      USAGE_COMPOSER_OVERLAY = 0x800, USAGE_PROTECTED_CONTENT = 0x4000, USAGE_VIDEO_ENCODE = 0x10000,
      USAGE_SENSOR_DIRECT_DATA = 0x800000, USAGE_GPU_DATA_BUFFER = 0x1000000, USAGE_GPU_CUBE_MAP = 0x2000000,
      USAGE_GPU_MIPMAP_COMPLETE = 0x4000000, USAGE_FRONT_BUFFER = 1L << 32;

  private static native long nativeCreate(int width, int height, int format, int layers, long usage);
  private static native boolean nativeSupported(int width, int height, int format, int layers, long usage);
  private static native void nativeAcquire(long buffer);
  private static native void nativeRelease(long buffer);
  // 0 width, 1 height, 2 format, 3 layers, 4 usage
  private static native long nativeDescribe(long buffer, int field);

  // A buffer nobody closed is released once unreachable, as AOSP's NativeAllocationRegistry does.
  private static final java.lang.ref.Cleaner CLEANER = java.lang.ref.Cleaner.create();
  private static final class Reference implements Runnable {
    long buffer;
    Reference(long buffer) { this.buffer = buffer; }
    @Override public synchronized void run() {
      if (buffer != 0) nativeRelease(buffer);
      buffer = 0;
    }
  }
  private long mNativeObject;
  private final Reference reference;
  private final java.lang.ref.Cleaner.Cleanable cleanable;

  private HardwareBuffer(long buffer) {
    mNativeObject = buffer;
    reference = new Reference(buffer);
    cleanable = CLEANER.register(this, reference);
  }
  // Takes a new reference to an existing buffer.
  public static HardwareBuffer ventureWrap(long buffer) {
    if (buffer == 0) return null;
    nativeAcquire(buffer);
    return new HardwareBuffer(buffer);
  }
  public synchronized long ventureNativeBuffer() { return mNativeObject; }

  public static HardwareBuffer create(int width, int height, int format, int layers, long usage) {
    if (width <= 0) throw new IllegalArgumentException("Invalid width " + width);
    if (height <= 0) throw new IllegalArgumentException("Invalid height " + height);
    if (layers <= 0) throw new IllegalArgumentException("Invalid layer count " + layers);
    if (format == BLOB && height != 1) throw new IllegalArgumentException("Height must be 1 when using the BLOB format");
    long buffer = nativeCreate(width, height, format, layers, usage);
    if (buffer == 0) throw new IllegalArgumentException("Unable to create a HardwareBuffer");
    return new HardwareBuffer(buffer);
  }
  public static boolean isSupported(int width, int height, int format, int layers, long usage) {
    if (width <= 0 || height <= 0 || layers <= 0) throw new IllegalArgumentException("Invalid size");
    return nativeSupported(width, height, format, layers, usage);
  }
  private long describe(int field) {
    long buffer = ventureNativeBuffer();
    if (buffer == 0) throw new IllegalStateException("This HardwareBuffer has been closed and its attributes cannot be obtained.");
    return nativeDescribe(buffer, field);
  }
  public int getWidth() { return (int) describe(0); }
  public int getHeight() { return (int) describe(1); }
  public int getFormat() { return (int) describe(2); }
  public int getLayers() { return (int) describe(3); }
  public long getUsage() { return describe(4); }
  public long getId() { return describe(-1); }
  public synchronized boolean isClosed() { return mNativeObject == 0; }
  @Override public synchronized void close() {
    if (mNativeObject != 0) {
      mNativeObject = 0;
      cleanable.clean();
    }
  }
}
