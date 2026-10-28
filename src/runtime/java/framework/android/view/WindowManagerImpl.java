package android.view;
public final class WindowManagerImpl implements WindowManager {
  private final android.widget.FrameLayout root;
  public WindowManagerImpl(android.content.Context c) {
    root = new android.widget.FrameLayout(c);
    root.attach();
  }
  public Display getDefaultDisplay() {
    return new Display();
  }
  public WindowMetrics getCurrentWindowMetrics() {
    var metrics = com.venture.runtime.RuntimeEnvironment.resources().getDisplayMetrics();
    return new WindowMetrics(new android.graphics.Rect(0, 0, metrics.widthPixels, metrics.heightPixels),
        new WindowInsets(new android.graphics.Rect()), metrics.density);
  }
  public WindowMetrics getMaximumWindowMetrics() { return getCurrentWindowMetrics(); }
  public void addView(View v, ViewGroup.LayoutParams p) {
    root.addView(v, p);
  }
  public void removeView(View v) {
    root.removeView(v);
  }
  public void updateViewLayout(View v, ViewGroup.LayoutParams p) {
    v.setLayoutParams(p);
    v.requestLayout();
  }
}
