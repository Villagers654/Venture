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
    public int x, y;
    public float alpha = 1, dimAmount, horizontalMargin, verticalMargin;
    public float screenBrightness = -1;
    public float preferredRefreshRate;
    public int preferredDisplayModeId;
    public LayoutParams() {
      super(-1, -1);
    }
    // The public window flags (API 32). A game's DEX reads them as fields
    // (Medieval Dynasty stopped on FLAG_DIM_BEHIND).
    public static final int FLAG_ALLOW_LOCK_WHILE_SCREEN_ON = 0x1, FLAG_DIM_BEHIND = 0x2, FLAG_BLUR_BEHIND = 0x4,
        FLAG_NOT_FOCUSABLE = 0x8, FLAG_NOT_TOUCHABLE = 0x10, FLAG_NOT_TOUCH_MODAL = 0x20,
        FLAG_TOUCHABLE_WHEN_WAKING = 0x40, FLAG_KEEP_SCREEN_ON = 0x80, FLAG_LAYOUT_IN_SCREEN = 0x100,
        FLAG_LAYOUT_NO_LIMITS = 0x200, FLAG_FULLSCREEN = 0x400, FLAG_FORCE_NOT_FULLSCREEN = 0x800,
        FLAG_DITHER = 0x1000, FLAG_SECURE = 0x2000, FLAG_SCALED = 0x4000, FLAG_IGNORE_CHEEK_PRESSES = 0x8000,
        FLAG_LAYOUT_INSET_DECOR = 0x10000, FLAG_ALT_FOCUSABLE_IM = 0x20000, FLAG_WATCH_OUTSIDE_TOUCH = 0x40000,
        FLAG_SHOW_WHEN_LOCKED = 0x80000, FLAG_SHOW_WALLPAPER = 0x100000, FLAG_TURN_SCREEN_ON = 0x200000,
        FLAG_DISMISS_KEYGUARD = 0x400000, FLAG_SPLIT_TOUCH = 0x800000, FLAG_HARDWARE_ACCELERATED = 0x1000000,
        FLAG_LAYOUT_IN_OVERSCAN = 0x2000000, FLAG_TRANSLUCENT_STATUS = 0x4000000,
        FLAG_TRANSLUCENT_NAVIGATION = 0x8000000, FLAG_LOCAL_FOCUS_MODE = 0x10000000,
        FLAG_LAYOUT_ATTACHED_IN_DECOR = 0x40000000, FLAG_DRAWS_SYSTEM_BAR_BACKGROUNDS = 0x80000000;
  }
}
