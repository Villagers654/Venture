package android.widget;
public class RelativeLayout extends android.view.ViewGroup {
  public RelativeLayout(android.content.Context c) {
    super(c);
  }
  public RelativeLayout(android.content.Context c, android.util.AttributeSet attrs) {
    super(c, attrs);
  }
  public static class LayoutParams extends android.view.ViewGroup.MarginLayoutParams {
    private final java.util.Map<Integer, Integer> rules = new java.util.HashMap<>();
    public LayoutParams(int w, int h) {
      super(w, h);
    }
    public void addRule(int verb) {
      rules.put(verb, -1);
    }
    public void addRule(int verb, int anchor) {
      rules.put(verb, anchor);
    }
  }
}
