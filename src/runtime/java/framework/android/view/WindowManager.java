package android.view;
public interface WindowManager extends ViewManager {
  class BadTokenException extends android.util.AndroidRuntimeException {
    public BadTokenException() {}
    public BadTokenException(String name) { super(name); }
  }
  class InvalidDisplayException extends android.util.AndroidRuntimeException {
    public InvalidDisplayException() {}
    public InvalidDisplayException(String name) { super(name); }
  }
  Display getDefaultDisplay();
  WindowMetrics getCurrentWindowMetrics();
  WindowMetrics getMaximumWindowMetrics();
  void addView(View v, ViewGroup.LayoutParams p);
  void removeView(View v);
  void updateViewLayout(View v, ViewGroup.LayoutParams p);
  public static class LayoutParams extends ViewGroup.LayoutParams {
    public static final int LAYOUT_IN_DISPLAY_CUTOUT_MODE_DEFAULT = 0,
        LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES = 1, LAYOUT_IN_DISPLAY_CUTOUT_MODE_NEVER = 2,
        LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS = 3;
    public int flags, format = 1, type, gravity, softInputMode, layoutInDisplayCutoutMode;
    public float screenBrightness = -1;
    public float preferredRefreshRate;
    public int preferredDisplayModeId;
    public LayoutParams() {
      super(-1, -1);
    }
    public static final int FLAG_FULLSCREEN = 1024, FLAG_KEEP_SCREEN_ON = 128;
  }
}
