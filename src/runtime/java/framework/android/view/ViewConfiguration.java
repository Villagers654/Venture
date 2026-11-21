package android.view;

import android.content.Context;

/**
 * Density-scaled Android input policy; does not advertise a hardware menu key.
 */
public class ViewConfiguration {
  private final float density;
  public ViewConfiguration() {
    density = 1;
  }
  private ViewConfiguration(Context context) {
    density = context.getResources().getDisplayMetrics().density;
  }
  public static ViewConfiguration get(Context context) {
    return new ViewConfiguration(context);
  }
  private int scaled(int dp) {
    return Math.round(dp * density);
  }
  // Baseline values: AOSP core/java/android/view/ViewConfiguration.java (API
  // 32).
  public int getScaledTouchSlop() {
    return scaled(8);
  }
  public int getScaledPagingTouchSlop() {
    return scaled(16);
  }
  public int getScaledDoubleTapSlop() {
    return scaled(100);
  }
  public int getScaledWindowTouchSlop() {
    return scaled(16);
  }
  public int getScaledEdgeSlop() {
    return scaled(12);
  }
  public int getScaledMinimumFlingVelocity() {
    return scaled(50);
  }
  public int getScaledMaximumFlingVelocity() {
    return scaled(8000);
  }
  public int getScaledScrollBarSize() {
    return scaled(4);
  }
  public int getScaledMinimumScalingSpan() {
    return scaled(27);
  }
  public float getScaledHorizontalScrollFactor() {
    return scaled(64);
  }
  public float getScaledVerticalScrollFactor() {
    return scaled(64);
  }
  public int getScaledFadingEdgeLength() {
    return scaled(12);
  }
  public int getScaledMaximumDrawingCacheSize() {
    return 480 * 800 * 4;
  }
  public boolean hasPermanentMenuKey() {
    return false;
  }
  public boolean isFadingMarqueeEnabled() {
    return true;
  }
  public static int getTapTimeout() {
    return 100;
  }
  public static int getLongPressTimeout() {
    return 500;
  }
  public static int getDoubleTapTimeout() {
    return 300;
  }
  public static int getPressedStateDuration() {
    return 64;
  }
  public static int getScrollDefaultDelay() {
    return 300;
  }
  public static int getScrollBarFadeDuration() {
    return 250;
  }
  public static float getScrollFriction() {
    return 0.015f;
  }
  // AOSP's static (density-independent, mostly deprecated) forms of the values above.
  public static int getScrollBarSize() {
    return 4;
  }
  public static int getFadingEdgeLength() {
    return 12;
  }
  public static int getJumpTapTimeout() {
    return 500;
  }
  public static int getMultiPressTimeout() {
    return 300;
  }
  public static int getKeyRepeatTimeout() {
    return getLongPressTimeout();
  }
  public static int getKeyRepeatDelay() {
    return 50;
  }
  public static int getEdgeSlop() {
    return 12;
  }
  public static int getTouchSlop() {
    return 8;
  }
  public static int getWindowTouchSlop() {
    return 16;
  }
  public static int getMinimumFlingVelocity() {
    return 50;
  }
  public static int getMaximumFlingVelocity() {
    return 8000;
  }
  public static int getMaximumDrawingCacheSize() {
    return 480 * 800 * 4;
  }
  public static long getZoomControlsTimeout() {
    return 3000;
  }
  public static long getGlobalActionKeyTimeout() {
    return 500;
  }
}
