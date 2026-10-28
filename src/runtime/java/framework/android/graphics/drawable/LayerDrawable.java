package android.graphics.drawable;
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
}
