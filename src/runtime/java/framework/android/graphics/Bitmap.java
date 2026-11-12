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
  public enum CompressFormat { JPEG, PNG, WEBP, WEBP_LOSSY, WEBP_LOSSLESS }
  // Host image writers: PNG and JPEG (quality applied). Java has no WebP writer,
  // so WebP requests write PNG rather than nothing.
  public boolean compress(CompressFormat format, int quality, java.io.OutputStream stream) {
    if (recycled) throw new IllegalStateException("Can't compress a recycled bitmap");
    if (quality < 0 || quality > 100) throw new IllegalArgumentException("quality must be 0..100");
    java.util.Objects.requireNonNull(stream);
    try {
      if (format != CompressFormat.JPEG) return javax.imageio.ImageIO.write(image, "png", stream);
      // JPEG has no alpha: Android composites onto black.
      var rgb = new java.awt.image.BufferedImage(getWidth(), getHeight(), java.awt.image.BufferedImage.TYPE_INT_RGB);
      var g = rgb.createGraphics();
      try { g.drawImage(image, 0, 0, null); } finally { g.dispose(); }
      var writer = javax.imageio.ImageIO.getImageWritersByFormatName("jpeg").next();
      var params = writer.getDefaultWriteParam();
      params.setCompressionMode(javax.imageio.ImageWriteParam.MODE_EXPLICIT);
      params.setCompressionQuality(quality / 100f);
      try (var out = javax.imageio.ImageIO.createImageOutputStream(stream)) {
        writer.setOutput(out);
        writer.write(null, new javax.imageio.IIOImage(rgb, null, null), params);
      } finally {
        writer.dispose();
      }
      return true;
    } catch (java.io.IOException e) {
      return false;
    }
  }
  private int density = 160;
  public void setDensity(int density) { this.density = density; }
  public int getDensity() { return density; }
  public static Bitmap createScaledBitmap(Bitmap src, int width, int height, boolean filter) {
    if (width <= 0 || height <= 0) throw new IllegalArgumentException("width and height must be > 0");
    var scaled = new java.awt.image.BufferedImage(width, height, java.awt.image.BufferedImage.TYPE_INT_ARGB);
    var g = scaled.createGraphics();
    try {
      g.setRenderingHint(java.awt.RenderingHints.KEY_INTERPOLATION, filter
          ? java.awt.RenderingHints.VALUE_INTERPOLATION_BILINEAR : java.awt.RenderingHints.VALUE_INTERPOLATION_NEAREST_NEIGHBOR);
      g.drawImage(src.image, 0, 0, width, height, null);
    } finally {
      g.dispose();
    }
    var result = new Bitmap(scaled);
    result.density = src.density;
    return result;
  }
  // ARGB_8888 pixels in Android's memory order: R, G, B, A bytes, premultiplied.
  public void copyPixelsToBuffer(java.nio.Buffer dst) {
    if (recycled) throw new IllegalStateException("Bitmap recycled");
    int w = getWidth(), h = getHeight();
    if (!(dst instanceof java.nio.ByteBuffer bytes)) throw new RuntimeException("unsupported Buffer subclass");
    if (bytes.remaining() < w * h * 4) throw new RuntimeException("Buffer not large enough for pixels");
    int[] row = new int[w];
    for (int y = 0; y < h; ++y) {
      image.getRGB(0, y, w, 1, row, 0, w);
      for (int p : row) {
        int a = p >>> 24;
        bytes.put((byte) (((p >> 16) & 255) * a / 255)).put((byte) (((p >> 8) & 255) * a / 255))
            .put((byte) ((p & 255) * a / 255)).put((byte) a);
      }
    }
  }
}
