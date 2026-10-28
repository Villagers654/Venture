package android.graphics;
public final class Bitmap {
  public enum Config { ALPHA_8, RGB_565, ARGB_4444, ARGB_8888, RGBA_F16, HARDWARE }
  final java.awt.image.BufferedImage image;
  private final boolean mutable;
  private boolean recycled;
  public Bitmap(java.awt.image.BufferedImage i) {
    this(i, true);
  }
  private Bitmap(java.awt.image.BufferedImage i, boolean mutable) {
    image = java.util.Objects.requireNonNull(i);
    this.mutable = mutable;
  }
  public int getWidth() {
    return image.getWidth();
  }
  public int getHeight() {
    return image.getHeight();
  }
  public boolean isRecycled() {
    return recycled;
  }
  public boolean isMutable() {
    return mutable;
  }
  public void recycle() {
    recycled = true;
    image.flush();
  }
  public int getPixel(int x, int y) {
    if (recycled)
      throw new IllegalStateException("Bitmap recycled");
    return image.getRGB(x, y);
  }
  public void setPixel(int x, int y, int color) {
    if (recycled)
      throw new IllegalStateException("Bitmap recycled");
    if (!mutable)
      throw new IllegalStateException("Bitmap immutable");
    image.setRGB(x, y, color);
  }
  public void setPixels(int[] pixels, int offset, int stride, int x, int y, int width, int height) {
    if (recycled) throw new IllegalStateException("Bitmap recycled");
    if (!mutable) throw new IllegalStateException("Bitmap immutable");
    image.setRGB(x, y, width, height, pixels, offset, stride);
  }
  public static Bitmap createBitmap(int[] colors, int width, int height, Config config) {
    return createBitmap(colors, 0, width, width, height, config);
  }
  public static Bitmap createBitmap(
      int[] colors, int offset, int stride, int width, int height, Config config) {
    // AOSP graphics/java/android/graphics/Bitmap.java: color-array factories
    // copy into an immutable bitmap and allow a negative source row stride.
    if (width <= 0 || height <= 0 || Math.abs((long) stride) < width)
      throw new IllegalArgumentException("Invalid bitmap dimensions or stride");
    java.util.Objects.requireNonNull(colors);
    long lastRow = (long) offset + (long) (height - 1) * stride;
    if (offset < 0 || (long) offset + width > colors.length || lastRow < 0
        || lastRow + width > colors.length)
      throw new ArrayIndexOutOfBoundsException("Bitmap colors exceed array");
    Bitmap bitmap = createBitmap(width, height, config);
    bitmap.image.setRGB(0, 0, width, height, colors, offset, stride);
    return new Bitmap(bitmap.image, false);
  }
  public static Bitmap createBitmap(int width, int height, Config config) {
    if (config != Config.ARGB_8888)
      throw new UnsupportedOperationException("Bitmap config " + config);
    return new Bitmap(new java.awt.image.BufferedImage(
        width, height, java.awt.image.BufferedImage.TYPE_INT_ARGB));
  }
}
