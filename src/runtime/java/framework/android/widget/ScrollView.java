package android.widget;
// A vertically scrolling FrameLayout, as HorizontalScrollView is horizontally. Games build it
// for text panels and dialogs (Max Payne Mobile's VR port creates one at start).
public class ScrollView extends FrameLayout {
  private boolean fillViewport, smoothScrolling = true;
  public ScrollView(android.content.Context c) { super(c); }
  public ScrollView(android.content.Context c, android.util.AttributeSet a) { super(c, a); }
  public ScrollView(android.content.Context c, android.util.AttributeSet a, int style) { super(c, a, style); }
  public ScrollView(android.content.Context c, android.util.AttributeSet a, int style, int res) { super(c, a, style); }
  public void setFillViewport(boolean value) { fillViewport = value; }
  public boolean isFillViewport() { return fillViewport; }
  public void setSmoothScrollingEnabled(boolean value) { smoothScrolling = value; }
  public boolean isSmoothScrollingEnabled() { return smoothScrolling; }
  public boolean fullScroll(int direction) { return false; }
  public boolean pageScroll(int direction) { return false; }
  public boolean arrowScroll(int direction) { return false; }
  public final void smoothScrollTo(int x, int y) {}
  public final void smoothScrollBy(int dx, int dy) {}
}
