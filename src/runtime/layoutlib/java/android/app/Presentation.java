package android.app;

import android.content.Context;
import android.content.ContextWrapper;
import android.content.res.Resources;
import android.hardware.display.DisplayManager;
import android.os.Handler;
import android.os.Looper;
import android.util.TypedValue;
import android.view.Display;
import android.view.Gravity;
import android.view.Window;
import android.view.WindowManager;
import android.view.WindowManagerImpl;
import com.venture.runtime.layoutlib.Panel;

/**
 * AOSP android.app.Presentation, which layoutlib does not ship: a dialog shown
 * on another display. Its context is that display's: the window manager adds
 * its windows to the display's panel (a virtual display's panel draws into the
 * display's Surface), and its resources keep the outer context's. As on
 * Android, it dismisses itself when its display goes away.
 */
public class Presentation extends Dialog {
  private final Display mDisplay;
  private final DisplayManager mDisplayManager;
  private final Handler mHandler = new Handler(Looper.getMainLooper());
  private final DisplayManager.DisplayListener mDisplayListener = new DisplayManager.DisplayListener() {
    @Override public void onDisplayAdded(int displayId) {}
    @Override public void onDisplayRemoved(int displayId) {
      if (displayId == mDisplay.getDisplayId()) handleDisplayRemoved();
    }
    @Override public void onDisplayChanged(int displayId) {
      if (displayId == mDisplay.getDisplayId()) Presentation.this.onDisplayChanged();
    }
  };

  public Presentation(Context outerContext, Display display) { this(outerContext, display, 0); }

  public Presentation(Context outerContext, Display display, int theme) {
    this(outerContext, display, theme, WindowManager.LayoutParams.TYPE_PRESENTATION);
  }

  public Presentation(Context outerContext, Display display, int theme, int type) {
    super(displayContext(outerContext, display), theme(outerContext, theme), false);
    mDisplay = display;
    mDisplayManager = outerContext.getSystemService(DisplayManager.class);
    Window w = getWindow();
    w.setGravity(Gravity.FILL);
    w.setType(type);
    setCanceledOnTouchOutside(false);
  }

  private static int theme(Context outer, int theme) {
    if (theme != 0) return theme;
    TypedValue value = new TypedValue();
    outer.getTheme().resolveAttribute(com.android.internal.R.attr.presentationTheme, value, true);
    return value.resourceId;
  }

  // Dialog(Context, int, false) keeps this context; wrap the theme here instead.
  private static Context displayContext(Context outer, Display display) {
    if (outer == null) throw new IllegalArgumentException("outerContext must not be null");
    if (display == null) throw new IllegalArgumentException("display must not be null");
    return new android.view.ContextThemeWrapper(new DisplayContext(outer, display), theme(outer, 0));
  }

  public Display getDisplay() { return mDisplay; }

  public Resources getResources() { return getContext().getResources(); }

  public void onDisplayRemoved() {}

  public void onDisplayChanged() {}

  @Override
  public void show() {
    if (Panel.forDisplay(mDisplay.getDisplayId()) == null)
      throw new WindowManager.InvalidDisplayException("The display was removed");
    super.show();
  }

  @Override
  protected void onStart() {
    super.onStart();
    if (mDisplayManager != null) mDisplayManager.registerDisplayListener(mDisplayListener, mHandler);
  }

  @Override
  protected void onStop() {
    if (mDisplayManager != null) mDisplayManager.unregisterDisplayListener(mDisplayListener);
    super.onStop();
  }

  private void handleDisplayRemoved() {
    onDisplayRemoved();
    cancel();
  }

  /** The outer context seen from another display: that display's window manager. */
  private static final class DisplayContext extends ContextWrapper {
    private final Display display;
    private WindowManagerImpl windowManager;

    DisplayContext(Context base, Display display) {
      super(base);
      this.display = display;
    }

    @Override public Display getDisplay() { return display; }

    @Override public Display getDisplayNoVerify() { return display; }

    @Override
    public Object getSystemService(String name) {
      if (Context.WINDOW_SERVICE.equals(name)) {
        if (windowManager == null) {
          Panel panel = Panel.forDisplay(display.getDisplayId());
          windowManager = panel != null ? new WindowManagerImpl(this, panel) : new WindowManagerImpl(this);
        }
        return windowManager;
      }
      return super.getSystemService(name);
    }
  }
}
