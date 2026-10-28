package android.view;
public class Window {
  public interface Callback {
    default boolean dispatchKeyEvent(KeyEvent e) {
      return false;
    }
    default boolean dispatchKeyShortcutEvent(KeyEvent e) {
      return false;
    }
    default boolean dispatchTouchEvent(MotionEvent e) {
      return false;
    }
    default boolean dispatchTrackballEvent(MotionEvent e) {
      return false;
    }
    default boolean dispatchGenericMotionEvent(MotionEvent e) {
      return false;
    }
    default boolean dispatchPopulateAccessibilityEvent(
        android.view.accessibility.AccessibilityEvent e) {
      return false;
    }
    default View onCreatePanelView(int feature) {
      return null;
    }
    default boolean onCreatePanelMenu(int feature, Menu menu) {
      return false;
    }
    default boolean onPreparePanel(int feature, View view, Menu menu) {
      return false;
    }
    default boolean onMenuOpened(int feature, Menu menu) {
      return false;
    }
    default boolean onMenuItemSelected(int feature, MenuItem item) {
      return false;
    }
    default void onWindowAttributesChanged(WindowManager.LayoutParams p) {}
    default void onContentChanged() {}
    default void onWindowFocusChanged(boolean focused) {}
    default void onAttachedToWindow() {}
    default void onDetachedFromWindow() {}
    default void onPanelClosed(int feature, Menu menu) {}
    default boolean onSearchRequested() {
      return false;
    }
    default boolean onSearchRequested(SearchEvent e) {
      return onSearchRequested();
    }
    default ActionMode onWindowStartingActionMode(ActionMode.Callback callback) {
      return null;
    }
    default ActionMode onWindowStartingActionMode(ActionMode.Callback callback, int type) {
      return onWindowStartingActionMode(callback);
    }
    default void onActionModeStarted(ActionMode mode) {}
    default void onActionModeFinished(ActionMode mode) {}
    default void onProvideKeyboardShortcuts(
        java.util.List<KeyboardShortcutGroup> groups, Menu menu, int deviceId) {}
    default void onPointerCaptureChanged(boolean captured) {}
  }
  private Callback callback;
  public Callback getCallback() {
    return callback;
  }
  public void setCallback(Callback c) {
    callback = c;
  }
  private boolean decorFits = true;
  public void setDecorFitsSystemWindows(boolean fits) {
    decorFits = fits;
  }
  public boolean decorFitsSystemWindows() {
    return decorFits;
  }
  public static final int FEATURE_NO_TITLE = 1;
  private final android.widget.FrameLayout decor;
  private final WindowManager.LayoutParams attrs = new WindowManager.LayoutParams();
  private boolean closeOnTouchOutside;
  private SurfaceHolder.Callback2 surfaceCallback;
  public Window(android.content.Context c) {
    surfaceContext = c;
    if (c instanceof Callback cb)
      callback = cb;
    decor = new android.widget.FrameLayout(c);
    decor.setId(16908290);
  }
  public <T extends View> T findViewById(int id) {
    return decor.findViewById(id);
  }
  public void setBackgroundDrawable(android.graphics.drawable.Drawable background) {
    decor.setBackground(background);
  }
  public View getDecorView() {
    return decor;
  }
  public View peekDecorView() {
    return decor;
  }
  public WindowManager.LayoutParams getAttributes() {
    return attrs;
  }
  public void setAttributes(WindowManager.LayoutParams p) {
    attrs.flags = p.flags;
    attrs.format = p.format;
    attrs.screenBrightness = p.screenBrightness;
    attrs.softInputMode = p.softInputMode;
  }
  public void addFlags(int f) {
    attrs.flags |= f;
  }
  public void clearFlags(int f) {
    attrs.flags &= ~f;
  }
  public void setFlags(int f, int mask) {
    attrs.flags = (attrs.flags & ~mask) | (f & mask);
  }
  public void setFormat(int f) {
    attrs.format = f;
  }
  public void setSoftInputMode(int mode) {
    attrs.softInputMode = mode;
  }
  public void setCloseOnTouchOutside(boolean close) { closeOnTouchOutside = close; }
  public boolean shouldCloseOnTouch(android.content.Context context, MotionEvent event) {
    if (!closeOnTouchOutside) return false;
    if (event.getAction() == MotionEvent.ACTION_OUTSIDE) return true;
    if (event.getAction() != MotionEvent.ACTION_DOWN) return false;
    // AOSP Window.java: tolerate the density-scaled window slop around its decor.
    int slop = ViewConfiguration.get(context).getScaledWindowTouchSlop();
    boolean outside = event.getX() < -slop || event.getY() < -slop
        || event.getX() > decor.getWidth() + slop || event.getY() > decor.getHeight() + slop;
    return outside;
  }
  public boolean requestFeature(int feature) {
    return feature == FEATURE_NO_TITLE;
  }
  public void setContentView(View v) {
    decor.removeAllViews();
    decor.addView(v);
  }
  public LayoutInflater getLayoutInflater() {
    return LayoutInflater.from(surfaceContext);
  }
  public void setContentView(int resource) {
    // AOSP PhoneWindow.java: inflate into the content parent, supporting <merge>.
    decor.removeAllViews();
    getLayoutInflater().inflate(resource, decor);
    if (callback != null) callback.onContentChanged();
  }
  public void setContentView(View v, ViewGroup.LayoutParams p) {
    decor.removeAllViews();
    decor.addView(v, p);
  }
  private SurfaceView nativeSurface;
  private final android.content.Context surfaceContext;
  public void takeSurface(SurfaceHolder.Callback2 c) {
    if (nativeSurface != null) {
      nativeSurface.detach();
      nativeSurface = null;
    }
    surfaceCallback = c;
    if (c != null) {
      nativeSurface = new SurfaceView(surfaceContext);
      nativeSurface.getHolder().addCallback(c);
    }
  }
  public void attach() {
    // AOSP 14 ViewRootImpl: a visible window requests its thread's animators.
    android.animation.AnimationHandler.requestAnimatorsEnabled(true, this);
    decor.attach();
    if (nativeSurface != null)
      nativeSurface.attach();
  }
  public void detach() {
    if (nativeSurface != null)
      nativeSurface.detach();
    decor.detach();
    android.animation.AnimationHandler.requestAnimatorsEnabled(false, this);
  }
  public SurfaceHolder.Callback2 surfaceCallback() {
    return surfaceCallback;
  }
}
