package android.util;

/** Immutable dimensions, AOSP core/java/android/util/Size.java. */
public final class Size {
  private final int width, height;
  public Size(int width, int height) { this.width = width; this.height = height; }
  public int getWidth() { return width; }
  public int getHeight() { return height; }
  @Override public boolean equals(Object other) { return other instanceof Size s && width == s.width && height == s.height; }
  @Override public int hashCode() { return height ^ Integer.rotateLeft(width, 16); }
  @Override public String toString() { return width + "x" + height; }
  public static Size parseSize(String value) {
    java.util.Objects.requireNonNull(value, "string must not be null");
    int split = value.indexOf('*');
    if (split < 0) split = value.indexOf('x');
    if (split < 0) throw new NumberFormatException("Invalid Size: " + value);
    return new Size(Integer.parseInt(value.substring(0, split)), Integer.parseInt(value.substring(split + 1)));
  }
}
