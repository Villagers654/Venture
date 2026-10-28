package android.graphics;
public final class BitmapFactory {
  public static class Options {
    public boolean inJustDecodeBounds, inScaled = true;
    public int outWidth = -1, outHeight = -1, inSampleSize = 1, inDensity, inTargetDensity;
  }
  public static Bitmap decodeStream(java.io.InputStream in) {
    return decodeStream(in, null, null);
  }
  public static Bitmap decodeStream(java.io.InputStream in, Rect padding, Options options) {
    try {
      var image = javax.imageio.ImageIO.read(in);
      if (image == null)
        return null;
      if (options != null) {
        options.outWidth = image.getWidth();
        options.outHeight = image.getHeight();
        if (options.inJustDecodeBounds)
          return null;
        if (options.inSampleSize > 1) {
          var scaled =
              new java.awt.image.BufferedImage(Math.max(1, image.getWidth() / options.inSampleSize),
                  Math.max(1, image.getHeight() / options.inSampleSize),
                  java.awt.image.BufferedImage.TYPE_INT_ARGB);
          var g = scaled.createGraphics();
          try {
            g.drawImage(image, 0, 0, scaled.getWidth(), scaled.getHeight(), null);
          } finally {
            g.dispose();
          }
          image = scaled;
        }
      }
      return new Bitmap(image);
    } catch (java.io.IOException e) {
      android.util.Log.w("BitmapFactory", e);
      return null;
    }
  }
  public static Bitmap decodeResource(android.content.res.Resources r, int id) {
    try (var in = r.openRawResource(id)) {
      return decodeStream(in);
    } catch (java.io.IOException e) {
      return null;
    }
  }
}
