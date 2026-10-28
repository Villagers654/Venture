package android.widget;
public class FrameLayout extends android.view.ViewGroup {
  public FrameLayout(android.content.Context c) {
    super(c);
  }
  public FrameLayout(android.content.Context c, android.util.AttributeSet a) {
    super(c, a);
  }
  public FrameLayout(android.content.Context c, android.util.AttributeSet a, int s) {
    super(c, a, s);
  }
  public static class LayoutParams extends android.view.ViewGroup.MarginLayoutParams {
    public int gravity = -1;
    public LayoutParams(int w, int h) {
      super(w, h);
    }
    public LayoutParams(int w, int h, int g) {
      super(w, h);
      gravity = g;
    }
  }
}
