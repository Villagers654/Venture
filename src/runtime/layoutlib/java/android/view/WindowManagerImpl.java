package android.view;

import android.content.Context;
import android.graphics.Rect;
import android.os.IBinder;
import com.venture.runtime.layoutlib.Layoutlib;
import com.venture.runtime.layoutlib.Panel;

/**
 * The window manager of a 2D process's contexts: every window the app adds
 * (an activity's decor, a dialog, a popup, a toast) becomes a VentureWindow on
 * Venture's panel. Replaces AOSP's WindowManagerImpl, whose WindowManagerGlobal
 * reaches the system's window manager over Binder.
 */
public final class WindowManagerImpl implements WindowManager {
  private final Context mContext;
  private final Window mParentWindow;
  private final Panel mPanel;

  public WindowManagerImpl(Context context) { this(context, null, Panel.get()); }

  /** The window manager of a display's context: its windows go to that display's panel. */
  public WindowManagerImpl(Context context, Panel panel) { this(context, null, panel); }

  private WindowManagerImpl(Context context, Window parentWindow, Panel panel) {
    mContext = context;
    mParentWindow = parentWindow;
    mPanel = panel;
  }

  public WindowManagerImpl createLocalWindowManager(Window parentWindow) {
    return new WindowManagerImpl(mContext, parentWindow, mPanel);
  }

  public WindowManagerImpl createPresentationWindowManager(Context displayContext) {
    Panel panel = Panel.forDisplay(displayContext.getDisplay().getDisplayId());
    return new WindowManagerImpl(displayContext, mParentWindow, panel != null ? panel : mPanel);
  }

  public void setDefaultToken(IBinder token) {}

  @Override
  public void addView(View view, ViewGroup.LayoutParams params) {
    if (!(params instanceof WindowManager.LayoutParams lp)) throw new IllegalArgumentException("Params must be WindowManager.LayoutParams");
    WindowManager.LayoutParams copy = new WindowManager.LayoutParams();
    copy.copyFrom(lp);
    if (mParentWindow != null) mParentWindow.adjustLayoutParamsForSubWindow(copy);
    // As WindowManagerGlobal.addView: the root view's own layout params are the window's.
    view.setLayoutParams(copy);
    mPanel.addWindow(view, copy, mContext);
  }

  @Override
  public void updateViewLayout(View view, ViewGroup.LayoutParams params) {
    if (!(params instanceof WindowManager.LayoutParams lp)) throw new IllegalArgumentException("Params must be WindowManager.LayoutParams");
    view.setLayoutParams(lp);
    mPanel.updateWindow(view, lp);
  }

  @Override
  public void removeView(View view) { mPanel.removeWindow(view); }

  @Override
  public void removeViewImmediate(View view) { mPanel.removeWindow(view); }

  @Override
  public Display getDefaultDisplay() { return mPanel.display(); }

  @Override
  public void requestAppKeyboardShortcuts(KeyboardShortcutsReceiver receiver, int deviceId) {
    receiver.onKeyboardShortcutsReceived(java.util.List.of());
  }

  @Override
  public KeyboardShortcutGroup getApplicationLaunchKeyboardShortcuts(int deviceId) {
    return new KeyboardShortcutGroup("", java.util.List.of());
  }

  @Override
  public android.graphics.Region getCurrentImeTouchRegion() { return new android.graphics.Region(); }

  @Override
  public WindowMetrics getCurrentWindowMetrics() { return metrics(); }

  @Override
  public WindowMetrics getMaximumWindowMetrics() { return metrics(); }

  private WindowMetrics metrics() {
    Rect bounds = new Rect(0, 0, mPanel.width(), mPanel.height());
    android.util.DisplayMetrics metrics = new android.util.DisplayMetrics();
    mPanel.display().getMetrics(metrics);
    return new WindowMetrics(bounds, new WindowInsets.Builder().build(), metrics.density);
  }
}
