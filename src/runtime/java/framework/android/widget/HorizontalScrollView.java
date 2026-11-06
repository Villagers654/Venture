package android.widget;
public class HorizontalScrollView extends FrameLayout {
  private boolean fillViewport, smoothScrolling = true;
  public HorizontalScrollView(android.content.Context c) { super(c); }
  public HorizontalScrollView(android.content.Context c, android.util.AttributeSet a) { super(c, a); }
  public HorizontalScrollView(android.content.Context c, android.util.AttributeSet a, int style) { super(c, a, style); }
  public HorizontalScrollView(android.content.Context c, android.util.AttributeSet a, int style, int res) { super(c, a, style); }
  public void setFillViewport(boolean value) { fillViewport = value; }
  public boolean isFillViewport() { return fillViewport; }
  public void setSmoothScrollingEnabled(boolean value) { smoothScrolling = value; }
  public boolean isSmoothScrollingEnabled() { return smoothScrolling; }
}