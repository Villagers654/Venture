package android.graphics.drawable;

import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.PixelFormat;

public class LayerDrawable extends Drawable {
  private final Drawable[] layers;
  public LayerDrawable(Drawable[] d) {
    layers = d.clone();
  }
  public int getNumberOfLayers() {
    return layers.length;
  }
  public Drawable getDrawable(int index) {
    return layers[index];
  }
  public void setLayerInset(int index, int l, int t, int r, int b) {
    layers[index].setBounds(l, t, r, b);
  }
  public void draw(Canvas canvas) {
    for (Drawable layer : layers)
      if (layer != null) layer.draw(canvas);
  }
  public void setAlpha(int alpha) {
    for (Drawable layer : layers)
      if (layer != null) layer.setAlpha(alpha);
  }
  public int getAlpha() {
    for (Drawable layer : layers)
      if (layer != null) return layer.getAlpha();
    return super.getAlpha();
  }
  public void setColorFilter(ColorFilter filter) {
    for (Drawable layer : layers)
      if (layer != null) layer.setColorFilter(filter);
  }
  public int getOpacity() {
    int opacity = PixelFormat.TRANSPARENT;
    boolean first = true;
    for (Drawable layer : layers)
      if (layer != null) {
        opacity = first ? layer.getOpacity() : resolveOpacity(opacity, layer.getOpacity());
        first = false;
      }
    return opacity;
  }
  public boolean isStateful() {
    for (Drawable layer : layers)
      if (layer != null && layer.isStateful()) return true;
    return false;
  }
  protected boolean onStateChange(int[] state) {
    boolean changed = false;
    for (Drawable layer : layers)
      if (layer != null && layer.isStateful() && layer.setState(state)) changed = true;
    return changed;
  }
  protected boolean onLevelChange(int level) {
    boolean changed = false;
    for (Drawable layer : layers)
      if (layer != null && layer.setLevel(level)) changed = true;
    return changed;
  }
}
