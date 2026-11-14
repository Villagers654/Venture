package android.util;
/**
 * The Android display: a headset's two panels side by side (xr/display_override.h
 * android_display_size), set by the runtime from the host's eye size before the
 * application starts; 1920x1080 until then.
 */
public class DisplayMetrics {
  public static final int DENSITY_DEFAULT = 160, DENSITY_MEDIUM = 160, DENSITY_HIGH = 240,
                          DENSITY_XHIGH = 320;
  private static volatile int defaultWidth = 1920, defaultHeight = 1080;
  public int widthPixels = defaultWidth, heightPixels = defaultHeight, densityDpi = 320;
  public float density = 2, scaledDensity = 2, xdpi = 320, ydpi = 320;
  public static void ventureSetDisplaySize(int width, int height) {
    if (width <= 0 || height <= 0) throw new IllegalArgumentException("Invalid display size");
    defaultWidth = width;
    defaultHeight = height;
  }
  public static int ventureDisplayWidth() { return defaultWidth; }
  public static int ventureDisplayHeight() { return defaultHeight; }
  public void setTo(DisplayMetrics d) {
    widthPixels = d.widthPixels;
    heightPixels = d.heightPixels;
    densityDpi = d.densityDpi;
    density = d.density;
    scaledDensity = d.scaledDensity;
    xdpi = d.xdpi;
    ydpi = d.ydpi;
  }
  public void setToDefaults() {
    setTo(new DisplayMetrics());
  }
}
