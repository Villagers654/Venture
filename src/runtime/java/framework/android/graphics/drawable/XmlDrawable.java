package android.graphics.drawable;

// Resource XML is kept for the app; it is not rasterized.
public class XmlDrawable extends Drawable {
  public final com.venture.runtime.BinaryResources.Node definition;
  private int alpha = 255;
  private android.graphics.ColorFilter colorFilter;
  public XmlDrawable(com.venture.runtime.BinaryResources.Node root) {
    definition = root;
  }
  public void draw(android.graphics.Canvas canvas) {}
  public void setAlpha(int alpha) {
    this.alpha = alpha;
  }
  public int getAlpha() {
    return alpha;
  }
  public void setColorFilter(android.graphics.ColorFilter filter) {
    colorFilter = filter;
  }
  public android.graphics.ColorFilter getColorFilter() {
    return colorFilter;
  }
  public int getOpacity() {
    return android.graphics.PixelFormat.TRANSLUCENT;
  }
}
