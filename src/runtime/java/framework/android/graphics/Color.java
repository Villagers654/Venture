package android.graphics;

import java.util.HashMap;
import java.util.Locale;

// AOSP graphics/java/android/graphics/Color.java: packed sRGB ARGB ints and their
// helpers (API 1), plus the sRGB Color object (API 26). Launcher activities use
// the constants and parseColor in static initializers.
public class Color {
  public static final int BLACK = 0xFF000000;
  public static final int DKGRAY = 0xFF444444;
  public static final int GRAY = 0xFF888888;
  public static final int LTGRAY = 0xFFCCCCCC;
  public static final int WHITE = 0xFFFFFFFF;
  public static final int RED = 0xFFFF0000;
  public static final int GREEN = 0xFF00FF00;
  public static final int BLUE = 0xFF0000FF;
  public static final int YELLOW = 0xFFFFFF00;
  public static final int CYAN = 0xFF00FFFF;
  public static final int MAGENTA = 0xFFFF00FF;
  public static final int TRANSPARENT = 0;

  private final float r, g, b, a;

  public Color() { this(0f, 0f, 0f, 1f); }
  private Color(float r, float g, float b, float a) { this.r = r; this.g = g; this.b = b; this.a = a; }

  public static Color valueOf(int color) {
    return new Color(red(color) / 255f, green(color) / 255f, blue(color) / 255f, alpha(color) / 255f);
  }
  public static Color valueOf(long color) { return valueOf((int) (color >>> 32)); }
  public static Color valueOf(float r, float g, float b) { return new Color(r, g, b, 1f); }
  public static Color valueOf(float r, float g, float b, float a) { return new Color(r, g, b, a); }

  public float red() { return r; }
  public float green() { return g; }
  public float blue() { return b; }
  public float alpha() { return a; }
  public int getComponentCount() { return 4; }
  public float[] getComponents() { return new float[] {r, g, b, a}; }
  public float getComponent(int i) { return getComponents()[i]; }
  public boolean isSrgb() { return true; }
  public boolean isWideGamut() { return false; }
  public float luminance() { return luminance(toArgb()); }
  public int toArgb() { return argb(a, r, g, b); }
  public long pack() { return pack(toArgb()); }

  public static long pack(int color) { return (color & 0xffffffffL) << 32; }
  public static long pack(float r, float g, float b) { return pack(rgb(r, g, b)); }
  public static long pack(float r, float g, float b, float a) { return pack(argb(a, r, g, b)); }
  public static int toArgb(long color) { return (int) (color >>> 32); }
  public static boolean isSrgb(long color) { return true; }

  public static int alpha(int color) { return color >>> 24; }
  public static int red(int color) { return (color >> 16) & 0xFF; }
  public static int green(int color) { return (color >> 8) & 0xFF; }
  public static int blue(int color) { return color & 0xFF; }

  public static int rgb(int red, int green, int blue) { return 0xff000000 | (red << 16) | (green << 8) | blue; }
  public static int rgb(float red, float green, float blue) { return argb(1f, red, green, blue); }
  public static int argb(int alpha, int red, int green, int blue) {
    return (alpha << 24) | (red << 16) | (green << 8) | blue;
  }
  public static int argb(float alpha, float red, float green, float blue) {
    return ((int) (alpha * 255.0f + 0.5f) << 24) | ((int) (red * 255.0f + 0.5f) << 16)
        | ((int) (green * 255.0f + 0.5f) << 8) | (int) (blue * 255.0f + 0.5f);
  }

  // Relative luminance (sRGB, D65), as AOSP.
  public static float luminance(int color) {
    double r = linear(red(color) / 255.0), g = linear(green(color) / 255.0), b = linear(blue(color) / 255.0);
    return (float) (0.2126 * r + 0.7152 * g + 0.0722 * b);
  }
  private static double linear(double c) { return c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4); }

  public static int parseColor(String colorString) {
    if (colorString.charAt(0) == '#') {
      long color = Long.parseLong(colorString.substring(1), 16);
      if (colorString.length() == 7) color |= 0x00000000ff000000L;
      else if (colorString.length() != 9) throw new IllegalArgumentException("Unknown color");
      return (int) color;
    }
    Integer color = NAMES.get(colorString.toLowerCase(Locale.ROOT));
    if (color != null) return color;
    throw new IllegalArgumentException("Unknown color");
  }

  public static void RGBToHSV(int red, int green, int blue, float[] hsv) {
    if (hsv.length < 3) throw new RuntimeException("3 components required for hsv");
    float r = red / 255f, g = green / 255f, b = blue / 255f;
    float max = Math.max(r, Math.max(g, b)), min = Math.min(r, Math.min(g, b)), delta = max - min;
    float h = 0f;
    if (delta != 0f) {
      if (max == r) h = (g - b) / delta;
      else if (max == g) h = 2f + (b - r) / delta;
      else h = 4f + (r - g) / delta;
      h *= 60f;
      if (h < 0f) h += 360f;
    }
    hsv[0] = h;
    hsv[1] = max == 0f ? 0f : delta / max;
    hsv[2] = max;
  }
  public static void colorToHSV(int color, float[] hsv) { RGBToHSV(red(color), green(color), blue(color), hsv); }
  public static int HSVToColor(float[] hsv) { return HSVToColor(0xFF, hsv); }
  public static int HSVToColor(int alpha, float[] hsv) {
    if (hsv.length < 3) throw new RuntimeException("3 components required for hsv");
    float h = ((hsv[0] % 360f) + 360f) % 360f / 60f;
    float s = Math.max(0f, Math.min(1f, hsv[1])), v = Math.max(0f, Math.min(1f, hsv[2]));
    int sector = (int) Math.floor(h);
    float f = h - sector, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    float r, g, b;
    switch (sector) {
      case 0: r = v; g = t; b = p; break;
      case 1: r = q; g = v; b = p; break;
      case 2: r = p; g = v; b = t; break;
      case 3: r = p; g = q; b = v; break;
      case 4: r = t; g = p; b = v; break;
      default: r = v; g = p; b = q; break;
    }
    return (alpha << 24) | (Math.round(r * 255) << 16) | (Math.round(g * 255) << 8) | Math.round(b * 255);
  }

  @Override public boolean equals(Object o) {
    if (!(o instanceof Color)) return false;
    Color c = (Color) o;
    return c.r == r && c.g == g && c.b == b && c.a == a;
  }
  @Override public int hashCode() { return toArgb(); }
  @Override public String toString() { return "Color(" + r + ", " + g + ", " + b + ", " + a + ", sRGB IEC61966-2.1)"; }

  private static final HashMap<String, Integer> NAMES = new HashMap<>();
  static {
    NAMES.put("black", BLACK); NAMES.put("darkgray", DKGRAY); NAMES.put("gray", GRAY);
    NAMES.put("lightgray", LTGRAY); NAMES.put("white", WHITE); NAMES.put("red", RED);
    NAMES.put("green", GREEN); NAMES.put("blue", BLUE); NAMES.put("yellow", YELLOW);
    NAMES.put("cyan", CYAN); NAMES.put("magenta", MAGENTA); NAMES.put("aqua", 0xFF00FFFF);
    NAMES.put("fuchsia", 0xFFFF00FF); NAMES.put("darkgrey", DKGRAY); NAMES.put("grey", GRAY);
    NAMES.put("lightgrey", LTGRAY); NAMES.put("lime", 0xFF00FF00); NAMES.put("maroon", 0xFF800000);
    NAMES.put("navy", 0xFF000080); NAMES.put("olive", 0xFF808000); NAMES.put("purple", 0xFF800080);
    NAMES.put("silver", 0xFFC0C0C0); NAMES.put("teal", 0xFF008080);
  }
}
