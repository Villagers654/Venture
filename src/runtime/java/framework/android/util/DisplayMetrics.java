package android.util;
/** The Java window is a 1920x1080 logical mirror, not an HMD eye texture. */
public class DisplayMetrics {
  public static final int DENSITY_DEFAULT = 160, DENSITY_MEDIUM = 160, DENSITY_HIGH = 240,
                          DENSITY_XHIGH = 320;
  public int widthPixels = 1920, heightPixels = 1080, densityDpi = 320;
  public float density = 2, scaledDensity = 2, xdpi = 320, ydpi = 320;
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
