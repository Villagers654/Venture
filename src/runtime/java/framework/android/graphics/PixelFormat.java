package android.graphics;

// AOSP graphics/java/android/graphics/PixelFormat.java: surface formats and the
// opacity values Drawable.getOpacity reports.
public class PixelFormat {
  public static final int UNKNOWN = 0, TRANSLUCENT = -3, TRANSPARENT = -2, OPAQUE = -1;
  public static final int RGBA_8888 = 1, RGBX_8888 = 2, RGB_888 = 3, RGB_565 = 4;
  @Deprecated public static final int RGBA_5551 = 6, RGBA_4444 = 7, A_8 = 8, L_8 = 9, LA_88 = 0xA, RGB_332 = 0xB;
  @Deprecated public static final int YCbCr_422_SP = 0x10, YCbCr_420_SP = 0x11, YCbCr_422_I = 0x14, JPEG = 0x100;
  public static final int RGBA_F16 = 0x16, RGBA_1010102 = 0x2B;
  public int bytesPerPixel;
  public int bitsPerPixel;
  public static void getPixelFormatInfo(int format, PixelFormat info) {
    switch (format) {
      case RGBA_8888, RGBX_8888, RGBA_1010102 -> { info.bitsPerPixel = 32; info.bytesPerPixel = 4; }
      case RGB_888 -> { info.bitsPerPixel = 24; info.bytesPerPixel = 3; }
      case RGB_565, RGBA_5551, RGBA_4444, LA_88 -> { info.bitsPerPixel = 16; info.bytesPerPixel = 2; }
      case A_8, L_8, RGB_332 -> { info.bitsPerPixel = 8; info.bytesPerPixel = 1; }
      case YCbCr_422_SP, YCbCr_422_I -> { info.bitsPerPixel = 16; info.bytesPerPixel = 1; }
      case YCbCr_420_SP -> { info.bitsPerPixel = 12; info.bytesPerPixel = 1; }
      case RGBA_F16 -> { info.bitsPerPixel = 64; info.bytesPerPixel = 8; }
      default -> throw new IllegalArgumentException("unknown pixel format " + format);
    }
  }
  public static boolean formatHasAlpha(int format) {
    return switch (format) {
      case A_8, LA_88, RGBA_4444, RGBA_5551, RGBA_8888, RGBA_F16, RGBA_1010102, TRANSLUCENT, TRANSPARENT -> true;
      default -> false;
    };
  }
}
