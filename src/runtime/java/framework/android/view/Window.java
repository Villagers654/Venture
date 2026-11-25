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
  public static final int FEATURE_OPTIONS_PANEL = 0, FEATURE_NO_TITLE = 1, FEATURE_PROGRESS = 2,
      FEATURE_LEFT_ICON = 3, FEATURE_RIGHT_ICON = 4, FEATURE_INDETERMINATE_PROGRESS = 5,
      FEATURE_CONTEXT_MENU = 6, FEATURE_CUSTOM_TITLE = 7, FEATURE_ACTION_BAR = 8,
      FEATURE_ACTION_BAR_OVERLAY = 9, FEATURE_ACTION_MODE_OVERLAY = 10, FEATURE_SWIPE_TO_DISMISS = 11,
      FEATURE_CONTENT_TRANSITIONS = 12, FEATURE_ACTIVITY_TRANSITIONS = 13;
  // AOSP Window.getDefaultFeatures: the options panel and context menu. A Venture
  // window reads no theme, so the activity and content transition features are
  // on only when requested, as for a theme that leaves them off.
  private int features = (1 << FEATURE_OPTIONS_PANEL) | (1 << FEATURE_CONTEXT_MENU);
  public boolean hasFeature(int feature) { return (features & (1 << feature)) != 0; }
  protected final int getFeatures() { return features; }
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
  public View getCurrentFocus() {
    return decor.findFocus();
  }
  public final android.content.Context getContext() {
    return surfaceContext;
  }
  // API 24 frame statistics. Sentry's SentryFrameMetricsCollector implements the
  // listener in its constructor, so without the type its init callback failed
  // before the manifest DSN was read. The decor view has no hardware renderer
  // here (games draw through their own surfaces), so no frames are reported.
  public interface OnFrameMetricsAvailableListener {
    void onFrameMetricsAvailable(Window window, FrameMetrics frameMetrics, int dropCountSinceLastInvocation);
  }
  private final java.util.List<OnFrameMetricsAvailableListener> frameMetricsListeners =
      new java.util.concurrent.CopyOnWriteArrayList<>();
  public final void addOnFrameMetricsAvailableListener(
      OnFrameMetricsAvailableListener listener, android.os.Handler handler) {
    frameMetricsListeners.add(java.util.Objects.requireNonNull(listener));
  }
  // AOSP View.removeFrameMetricsListener rejects a listener that was never added.
  public final void removeOnFrameMetricsAvailableListener(OnFrameMetricsAvailableListener listener) {
    if (!frameMetricsListeners.remove(listener))
      throw new IllegalArgumentException(
          "attempt to remove OnFrameMetricsAvailableListener that was never added");
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
  // The features a Quest app window accepts and records; the title, progress
  // and action bar features change no decor here, so only no-title is accepted
  // among them.
  public boolean requestFeature(int feature) {
    if (feature != FEATURE_NO_TITLE && feature != FEATURE_CONTENT_TRANSITIONS && feature != FEATURE_ACTIVITY_TRANSITIONS)
      return false;
    features |= 1 << feature;
    if (feature == FEATURE_CONTENT_TRANSITIONS && transitionManager == null)
      transitionManager = new android.transition.TransitionManager();
    return true;
  }
  // AOSP Window transition state. Venture runs no window transitions (there is
  // no host renderer for Java views), so these are kept for the app to read
  // back. Return and reenter transitions default to the enter and exit ones.
  private static final android.transition.Transition USE_DEFAULT_TRANSITION = new android.transition.Fade();
  private android.transition.Transition enterTransition, exitTransition, sharedElementEnterTransition,
      sharedElementExitTransition;
  private android.transition.Transition returnTransition = USE_DEFAULT_TRANSITION,
      reenterTransition = USE_DEFAULT_TRANSITION, sharedElementReturnTransition = USE_DEFAULT_TRANSITION,
      sharedElementReenterTransition = USE_DEFAULT_TRANSITION;
  private Boolean allowEnterTransitionOverlap, allowReturnTransitionOverlap;
  private boolean sharedElementsUseOverlay = true;
  private long backgroundFadeDurationMillis = -1;
  private android.transition.TransitionManager transitionManager;
  private android.transition.Scene contentScene;
  public void setEnterTransition(android.transition.Transition transition) { enterTransition = transition; }
  public android.transition.Transition getEnterTransition() { return enterTransition; }
  public void setReturnTransition(android.transition.Transition transition) { returnTransition = transition; }
  public android.transition.Transition getReturnTransition() {
    return returnTransition == USE_DEFAULT_TRANSITION ? getEnterTransition() : returnTransition;
  }
  public void setExitTransition(android.transition.Transition transition) { exitTransition = transition; }
  public android.transition.Transition getExitTransition() { return exitTransition; }
  public void setReenterTransition(android.transition.Transition transition) { reenterTransition = transition; }
  public android.transition.Transition getReenterTransition() {
    return reenterTransition == USE_DEFAULT_TRANSITION ? getExitTransition() : reenterTransition;
  }
  public void setSharedElementEnterTransition(android.transition.Transition transition) { sharedElementEnterTransition = transition; }
  public android.transition.Transition getSharedElementEnterTransition() { return sharedElementEnterTransition; }
  public void setSharedElementReturnTransition(android.transition.Transition transition) { sharedElementReturnTransition = transition; }
  public android.transition.Transition getSharedElementReturnTransition() {
    return sharedElementReturnTransition == USE_DEFAULT_TRANSITION ? getSharedElementEnterTransition() : sharedElementReturnTransition;
  }
  public void setSharedElementExitTransition(android.transition.Transition transition) { sharedElementExitTransition = transition; }
  public android.transition.Transition getSharedElementExitTransition() { return sharedElementExitTransition; }
  public void setSharedElementReenterTransition(android.transition.Transition transition) { sharedElementReenterTransition = transition; }
  public android.transition.Transition getSharedElementReenterTransition() {
    return sharedElementReenterTransition == USE_DEFAULT_TRANSITION ? getSharedElementExitTransition() : sharedElementReenterTransition;
  }
  public void setAllowEnterTransitionOverlap(boolean allow) { allowEnterTransitionOverlap = allow; }
  public boolean getAllowEnterTransitionOverlap() { return allowEnterTransitionOverlap == null || allowEnterTransitionOverlap; }
  public void setAllowReturnTransitionOverlap(boolean allow) { allowReturnTransitionOverlap = allow; }
  public boolean getAllowReturnTransitionOverlap() { return allowReturnTransitionOverlap == null || allowReturnTransitionOverlap; }
  public void setTransitionBackgroundFadeDuration(long fadeDurationMillis) {
    if (fadeDurationMillis < 0) throw new IllegalArgumentException("negative durations are not allowed");
    backgroundFadeDurationMillis = fadeDurationMillis;
  }
  public long getTransitionBackgroundFadeDuration() {
    return backgroundFadeDurationMillis < 0 ? 300 : backgroundFadeDurationMillis;
  }
  public void setSharedElementsUseOverlay(boolean sharedElementsUseOverlay) { this.sharedElementsUseOverlay = sharedElementsUseOverlay; }
  public boolean getSharedElementsUseOverlay() { return sharedElementsUseOverlay; }
  public void setTransitionManager(android.transition.TransitionManager tm) { transitionManager = tm; }
  public android.transition.TransitionManager getTransitionManager() { return transitionManager; }
  public android.transition.Scene getContentScene() { return contentScene; }
  public void setContentView(View v) {
    if (hasFeature(FEATURE_CONTENT_TRANSITIONS)) {
      transitionTo(new android.transition.Scene(decor, v));
      return;
    }
    decor.removeAllViews();
    decor.addView(v);
  }
  // AOSP PhoneWindow.transitionTo: the first content scene enters directly, later
  // ones through the window's transition manager.
  private void transitionTo(android.transition.Scene scene) {
    if (contentScene == null || transitionManager == null) scene.enter();
    else transitionManager.transitionTo(scene);
    contentScene = scene;
  }
  public LayoutInflater getLayoutInflater() {
    return LayoutInflater.from(surfaceContext);
  }
  public void setContentView(int resource) {
    // AOSP PhoneWindow.java: inflate into the content parent, supporting <merge>;
    // with content transitions, through the layout's scene.
    if (hasFeature(FEATURE_CONTENT_TRANSITIONS)) {
      transitionTo(android.transition.Scene.getSceneForLayout(decor, resource, surfaceContext));
    } else {
      decor.removeAllViews();
      getLayoutInflater().inflate(resource, decor);
    }
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
  public void setLayout(int width, int height) {
    attrs.width = width;
    attrs.height = height;
  }
  public void setGravity(int gravity) { attrs.gravity = gravity; }
  public WindowManager getWindowManager() {
    return (WindowManager) surfaceContext.getSystemService(android.content.Context.WINDOW_SERVICE);
  }
  private final android.window.BackDispatcher backDispatcher = new android.window.BackDispatcher();
  public android.window.OnBackInvokedDispatcher getOnBackInvokedDispatcher() { return backDispatcher; }
  // A Quest window has no system bars to draw; the colors are kept for apps that read them back.
  private int statusBarColor, navigationBarColor, navigationBarDividerColor;
  private boolean statusBarContrastEnforced, navigationBarContrastEnforced = true;
  public void setStatusBarColor(int color) { statusBarColor = color; }
  public int getStatusBarColor() { return statusBarColor; }
  public void setNavigationBarColor(int color) { navigationBarColor = color; }
  public int getNavigationBarColor() { return navigationBarColor; }
  public void setNavigationBarDividerColor(int color) { navigationBarDividerColor = color; }
  public int getNavigationBarDividerColor() { return navigationBarDividerColor; }
  public void setStatusBarContrastEnforced(boolean enforced) { statusBarContrastEnforced = enforced; }
  public boolean isStatusBarContrastEnforced() { return statusBarContrastEnforced; }
  public void setNavigationBarContrastEnforced(boolean enforced) { navigationBarContrastEnforced = enforced; }
  public boolean isNavigationBarContrastEnforced() { return navigationBarContrastEnforced; }
}
