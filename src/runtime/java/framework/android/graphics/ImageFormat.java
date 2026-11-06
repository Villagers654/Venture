package android.graphics;

// AOSP graphics/java/android/graphics/ImageFormat.java: the formats Venture's
// images use, and their bits per pixel.
public class ImageFormat {
  public static final int UNKNOWN = 0, NV16 = 0x10, NV21 = 0x11, YUY2 = 0x14, JPEG = 0x100,
      YUV_420_888 = 0x23, PRIVATE = 0x22, YV12 = 0x32315659;
  public ImageFormat() {}
  public static int getBitsPerPixel(int format) {
    switch (format) {
      case NV16: case YUY2: return 16;
      case NV21: case YUV_420_888: case YV12: return 12;
      default: return -1;
    }
  }
}
