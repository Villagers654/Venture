package android.widget;
public class LinearLayout extends android.view.ViewGroup {
  private int orientation, gravity;
  public LinearLayout(android.content.Context c) {
    super(c);
  }
  public LinearLayout(android.content.Context c, android.util.AttributeSet a) {
    super(c, a);
  }
  public void setOrientation(int o) {
    orientation = o;
  }
  public int getOrientation() {
    return orientation;
  }
  public void setGravity(int g) {
    gravity = g;
  }
  public static class LayoutParams extends android.view.ViewGroup.MarginLayoutParams {
    public float weight;
    public int gravity;
    public LayoutParams(int w, int h) {
      super(w, h);
    }
    public LayoutParams(int w, int h, float weight) {
      super(w, h);
      this.weight = weight;
    }
  }
}
