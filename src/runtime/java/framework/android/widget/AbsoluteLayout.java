package android.widget;

import android.content.Context;
import android.util.AttributeSet;

/** AOSP android.widget.AbsoluteLayout: children placed at explicit x/y (WebView's superclass). */
@Deprecated
public class AbsoluteLayout extends android.view.ViewGroup {
  public AbsoluteLayout(Context context) { super(context); }
  public AbsoluteLayout(Context context, AttributeSet attrs) { super(context, attrs); }
  public AbsoluteLayout(Context context, AttributeSet attrs, int defStyleAttr) {
    super(context, attrs, defStyleAttr);
  }
  public AbsoluteLayout(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
    super(context, attrs, defStyleAttr);
  }
  public static class LayoutParams extends android.view.ViewGroup.LayoutParams {
    public int x, y;
    public LayoutParams(int width, int height, int x, int y) {
      super(width, height);
      this.x = x;
      this.y = y;
    }
    public LayoutParams(android.view.ViewGroup.LayoutParams source) { super(source); }
  }
}
