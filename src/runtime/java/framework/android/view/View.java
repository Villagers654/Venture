package android.view;
import android.content.Context;
import android.os.Handler;
import android.util.AttributeSet;
public class View implements ViewParent {
  public static final int VISIBLE = 0, INVISIBLE = 4, GONE = 8, NO_ID = -1,
                          SYSTEM_UI_FLAG_FULLSCREEN = 4, SYSTEM_UI_FLAG_HIDE_NAVIGATION = 2,
                          SYSTEM_UI_FLAG_LOW_PROFILE = 1, SYSTEM_UI_FLAG_LAYOUT_STABLE = 256,
                          SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION = 512,
                          SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN = 1024, SYSTEM_UI_FLAG_IMMERSIVE = 2048,
                          SYSTEM_UI_FLAG_IMMERSIVE_STICKY = 4096;
  private final Context context;
  private int id = -1, visibility, systemUi, width = 1920, height = 1080;
  private int left, top;
  private float translationX, translationY;
  public float getX() { return left + translationX; }
  public float getY() { return top + translationY; }
  public void setX(float x) { translationX = x - left; }
  public void setY(float y) { translationY = y - top; }
  public float getTranslationX() { return translationX; }
  public float getTranslationY() { return translationY; }
  public void setTranslationX(float x) { translationX = x; }
  public void setTranslationY(float y) { translationY = y; }
  public interface OnLayoutChangeListener {
    void onLayoutChange(View view, int left, int top, int right, int bottom, int oldLeft,
        int oldTop, int oldRight, int oldBottom);
  }
  public interface OnCapturedPointerListener {
    boolean onCapturedPointer(View view, MotionEvent event);
  }
  public interface OnGenericMotionListener {
    boolean onGenericMotion(View view, MotionEvent event);
  }
  private OnGenericMotionListener genericMotionListener;
  public void setOnGenericMotionListener(OnGenericMotionListener listener) {
    genericMotionListener = listener;
  }
  public boolean dispatchGenericMotionEvent(MotionEvent event) {
    return (genericMotionListener != null && genericMotionListener.onGenericMotion(this, event))
        || onGenericMotionEvent(event);
  }
  private OnCapturedPointerListener capturedPointerListener;
  public void setOnCapturedPointerListener(OnCapturedPointerListener listener) {
    capturedPointerListener = listener;
  }
  public boolean dispatchCapturedPointerEvent(MotionEvent event) {
    return capturedPointerListener != null
        && capturedPointerListener.onCapturedPointer(this, event);
  }
  private boolean captureUnavailableReported;
  public void requestPointerCapture() {
    // AOSP View.requestPointerCapture requests capture from ViewRootImpl; the
    // request does not itself grant capture. There is no host mouse adapter yet.
    if (attached && !captureUnavailableReported) {
      captureUnavailableReported = true;
      android.util.Log.w("View", "Pointer capture unavailable: no host mouse adapter");
    }
  }
  public boolean hasPointerCapture() {
    return false;
  }
  public void releasePointerCapture() {
    // No capture can be granted by this backend, so none needs releasing.
  }
  private PointerIcon pointerIcon;
  private boolean pointerIconUnavailableReported;
  public void setPointerIcon(PointerIcon icon) {
    pointerIcon = icon;
    if (attached && !pointerIconUnavailableReported) {
      pointerIconUnavailableReported = true;
      android.util.Log.w("View", "Pointer icon display unavailable: no host mouse adapter");
    }
  }
  public PointerIcon getPointerIcon() {
    return pointerIcon;
  }
  private final java.util.List<OnLayoutChangeListener> layoutListeners =
      new java.util.concurrent.CopyOnWriteArrayList<>();
  public void addOnLayoutChangeListener(OnLayoutChangeListener listener) {
    layoutListeners.add(listener);
  }
  public void removeOnLayoutChangeListener(OnLayoutChangeListener listener) {
    layoutListeners.remove(listener);
  }
  private boolean focus, attached, enabled = true, focusable, focusableInTouchMode, keepScreenOn,
                                   willNotDraw;
  private CharSequence description;
  // AOSP View.java, IMPORTANT_FOR_AUTOFILL_* (API 26): this is a view policy,
  // independent of whether the device supplies an AutofillManager service.
  public static final int IMPORTANT_FOR_AUTOFILL_AUTO = 0, IMPORTANT_FOR_AUTOFILL_YES = 1,
      IMPORTANT_FOR_AUTOFILL_NO = 2, IMPORTANT_FOR_AUTOFILL_YES_EXCLUDE_DESCENDANTS = 4,
      IMPORTANT_FOR_AUTOFILL_NO_EXCLUDE_DESCENDANTS = 8;
  private int importantForAutofill;
  public int getImportantForAutofill() { return importantForAutofill; }
  public void setImportantForAutofill(int mode) {
    if (mode != 0 && mode != 1 && mode != 2 && mode != 4 && mode != 8)
      throw new IllegalArgumentException("Invalid autofill importance mode");
    importantForAutofill = mode;
  }
  private ViewGroup.LayoutParams params;
  ViewParent parent;
  private OnSystemUiVisibilityChangeListener systemListener;
  private OnFocusChangeListener focusListener;
  private final ViewTreeObserver observer = new ViewTreeObserver();
  private Handler handler;
  public View(Context c) {
    context = c;
  }
  public View(Context c, AttributeSet a) {
    this(c);
  }
  public View(Context c, AttributeSet a, int style) {
    this(c);
  }
  public View(Context c, AttributeSet a, int style, int res) {
    this(c);
  }
  public Context getContext() {
    return context;
  }
  // AOSP View: IDE layout previews override this; an application view is not a preview.
  public boolean isInEditMode() { return false; }
  // AOSP android12-release View.java gates these inspection records on
  // sDebugViewAttributes. Venture's Settings.Global does not enable
  // debug_view_attributes or debug_view_attributes_application_package.
  // In that mode Android returns before even reading the supplied TypedArray.
  public final void saveAttributeDataForStyleable(Context context, int[] styleable,
      AttributeSet attrs, android.content.res.TypedArray values, int defStyleAttr, int defStyleRes) {
  }
  public java.util.Map<Integer, Integer> getAttributeSourceResourceMap() {
    return new java.util.HashMap<>();
  }
  public int[] getAttributeResolutionStack(int attribute) { return new int[0]; }
  public android.content.res.Resources getResources() {
    return context.getResources();
  }
  public ViewParent getParent() {
    return parent;
  }
  public View getRootView() {
    return parent instanceof View v ? v.getRootView() : this;
  }
  public void requestLayout() {
    layoutRequested = true;
    observer.dispatchTraversal();
  }
  private boolean layoutRequested;
  public void forceLayout() {
    layoutRequested = true;
  }
  public boolean isLayoutRequested() {
    return layoutRequested;
  }
  public void invalidate() {
    if (!willNotDraw)
      com.venture.runtime.RuntimeEnvironment.trace(
          "View.invalidate: Java canvas drawing unavailable");
  }
  public int getWidth() {
    return width;
  }
  public int getLeft() {
    return left;
  }
  public int getTop() {
    return top;
  }
  public int getRight() {
    return left + width;
  }
  public int getBottom() {
    return top + height;
  }
  public void getLocationInWindow(int[] location) {
    if (location == null || location.length < 2) throw new IllegalArgumentException("Location needs two coordinates");
    float x = 0, y = 0;
    for (View view = this; view != null; view = view.parent instanceof View p ? p : null) {
      x += view.getX(); y += view.getY();
    }
    location[0] = Math.round(x); location[1] = Math.round(y);
  }
  public void getLocationOnScreen(int[] location) {
    // Venture's logical Android display has one origin-aligned app window.
    getLocationInWindow(location);
  }
  public void getWindowVisibleDisplayFrame(android.graphics.Rect out) {
    // AOSP 12L View.java: this is the display frame, not this view's bounds.
    // Our logical app window fills the display; no system bars or rendered IME
    // reduce its visible area.
    Display display = getDisplay();
    out.set(0, 0, display.getWidth(), display.getHeight());
  }
  public void getDrawingRect(android.graphics.Rect out) {
    // AOSP 12L View.java: local drawing bounds exclude layout/translation.
    // Hosted views currently have no scroll offset.
    out.set(0, 0, width, height);
  }
  public int getHeight() {
    return height;
  }
  public int getMeasuredWidth() {
    return measuredWidth & MEASURED_SIZE_MASK;
  }
  public int getMeasuredHeight() {
    return measuredHeight & MEASURED_SIZE_MASK;
  }
  // AOSP 12L View.MeasureSpec: the high two bits carry mode, the low 30 size.
  public static final int MEASURED_SIZE_MASK = 0x00ffffff, MEASURED_STATE_MASK = 0xff000000,
      MEASURED_HEIGHT_STATE_SHIFT = 16, MEASURED_STATE_TOO_SMALL = 0x01000000;
  private int measuredWidth, measuredHeight;
  private boolean measuredOnce;
  public static class MeasureSpec {
    public static final int UNSPECIFIED = 0, EXACTLY = 0x40000000, AT_MOST = 0x80000000;
    public static int makeMeasureSpec(int size, int mode) { return (size & 0x3fffffff) | (mode & 0xc0000000); }
    public static int getMode(int spec) { return spec & 0xc0000000; }
    public static int getSize(int spec) { return spec & 0x3fffffff; }
    public static String toString(int spec) {
      return "MeasureSpec: " + switch (getMode(spec)) {
        case UNSPECIFIED -> "UNSPECIFIED "; case EXACTLY -> "EXACTLY "; case AT_MOST -> "AT_MOST ";
        default -> Integer.toString(getMode(spec)) + " ";
      } + getSize(spec);
    }
  }
  public final void measure(int widthSpec, int heightSpec) { onMeasure(widthSpec, heightSpec); }
  protected void onMeasure(int widthSpec, int heightSpec) {
    setMeasuredDimension(getDefaultSize(getSuggestedMinimumWidth(), widthSpec),
                         getDefaultSize(getSuggestedMinimumHeight(), heightSpec));
  }
  protected final void setMeasuredDimension(int width, int height) {
    measuredWidth = width; measuredHeight = height; measuredOnce = true;
  }
  public int getMeasuredWidthAndState() { return measuredWidth; }
  public int getMeasuredHeightAndState() { return measuredHeight; }
  public int getMeasuredState() { return (measuredWidth & MEASURED_STATE_MASK) | ((measuredHeight >> 16) & 0xff00); }
  protected int getSuggestedMinimumWidth() { return minWidth; }
  protected int getSuggestedMinimumHeight() { return minHeight; }
  public static int getDefaultSize(int size, int spec) { return MeasureSpec.getMode(spec) == 0 ? size : MeasureSpec.getSize(spec); }
  public static int resolveSize(int size, int spec) { return resolveSizeAndState(size, spec, 0) & MEASURED_SIZE_MASK; }
  public static int resolveSizeAndState(int size, int spec, int state) {
    int limit = MeasureSpec.getSize(spec), mode = MeasureSpec.getMode(spec);
    int result = mode == MeasureSpec.EXACTLY ? limit
        : mode == MeasureSpec.AT_MOST && limit < size ? limit | MEASURED_STATE_TOO_SMALL : size;
    return result | (state & MEASURED_STATE_MASK);
  }
  public static int combineMeasuredStates(int current, int next) { return current | next; }
  public void layout(int l, int t, int r, int b) {
    layoutRequested = false;
    int ow = width, oh = height;
    int ol = left, ot = top;
    left = l;
    top = t;
    width = r - l;
    height = b - t;
    onSizeChanged(width, height, ow, oh);
    if (ol != l || ot != t || ow != width || oh != height)
      for (var listener : layoutListeners)
        listener.onLayoutChange(this, l, t, r, b, ol, ot, ol + ow, ot + oh);
  }
  protected void onSizeChanged(int w, int h, int oldw, int oldh) {}
  public void setId(int value) {
    id = value;
  }
  public int getId() {
    return id;
  }
  @SuppressWarnings("unchecked")
  public <T extends View> T findViewById(int id) {
    return this.id == id ? (T) this : null;
  }
  public void setVisibility(int v) {
    visibility = v;
  }
  public int getVisibility() {
    return visibility;
  }
  public boolean isShown() {
    return visibility == VISIBLE && attached;
  }
  public void setEnabled(boolean e) {
    enabled = e;
  }
  public boolean isEnabled() {
    return enabled;
  }
  public void setContentDescription(CharSequence d) {
    description = d;
  }
  public CharSequence getContentDescription() {
    return description;
  }
  public boolean requestFocus() {
    focus = true;
    if (focusListener != null)
      focusListener.onFocusChange(this, true);
    return true;
  }
  public void clearFocus() {
    focus = false;
    if (focusListener != null)
      focusListener.onFocusChange(this, false);
  }
  public boolean hasFocus() {
    return focus;
  }
  public boolean isFocused() {
    return focus;
  }
  public void setFocusable(boolean b) {
    focusable = b;
  }
  public boolean isFocusable() { return focusable; }
  public void setFocusableInTouchMode(boolean b) {
    focusableInTouchMode = b;
  }
  public void setKeepScreenOn(boolean b) {
    keepScreenOn = b;
  }
  public void setWillNotDraw(boolean b) {
    willNotDraw = b;
  }
  public void setLayoutParams(ViewGroup.LayoutParams p) {
    params = p;
  }
  public ViewGroup.LayoutParams getLayoutParams() {
    return params;
  }
  public Handler getHandler() {
    if (handler == null)
      handler = new Handler(context.getMainLooper());
    return handler;
  }
  public boolean post(Runnable r) {
    return getHandler().post(r);
  }
  public boolean postDelayed(Runnable r, long d) {
    return getHandler().postDelayed(r, d);
  }
  public boolean removeCallbacks(Runnable r) {
    getHandler().removeCallbacks(r);
    return true;
  }
  public void setSystemUiVisibility(int v) {
    // AOSP 12L View.java: this records requested flags. The listener observes
    // system visibility changes, not this setter (UE4 reapplies flags there).
    systemUi = v;
  }
  public int getSystemUiVisibility() {
    return systemUi;
  }
  public void setOnSystemUiVisibilityChangeListener(OnSystemUiVisibilityChangeListener l) {
    systemListener = l;
  }
  public void setOnFocusChangeListener(OnFocusChangeListener l) {
    focusListener = l;
  }
  public ViewTreeObserver getViewTreeObserver() {
    return observer;
  }
  public Display getDisplay() {
    return new Display();
  }
  public boolean isAttachedToWindow() {
    return attached;
  }
  private final android.os.IBinder windowToken = new android.os.Binder();
  public android.os.IBinder getWindowToken() {
    return attached ? getRootView().windowToken : null;
  }
  public android.os.IBinder getApplicationWindowToken() {
    return getWindowToken();
  }
  public void attach() {
    if (attached)
      return;
    attached = true;
    onAttachedToWindow();
    for (var l : attachListeners) l.onViewAttachedToWindow(this);
    // ViewRootImpl.performTraversals measures and dispatches global layout on
    // the traversal after attachment and surface creation, so measured sizes
    // are still zero in onAttachedToWindow and right after addView. Defer until
    // the whole hosted hierarchy is attached; AGDK uses this callback to publish
    // its initial native content rectangle.
    post(() -> {
      if (!attached)
        return;
      seedMeasurement();
      observer.dispatchTraversal();
    });
  }
  // Stand in for the traversal's measure pass over this attached subtree.
  // Explicit guest measurement wins; exact LayoutParams sizes are what an
  // EXACTLY child spec yields, otherwise the hosted bounds are the result.
  void seedMeasurement() {
    if (measuredOnce)
      return;
    measuredWidth = params != null && params.width >= 0 ? params.width : width;
    measuredHeight = params != null && params.height >= 0 ? params.height : height;
  }
  public void detach() {
    if (!attached)
      return;
    onDetachedFromWindow();
    attached = false;
    for (var l : attachListeners) l.onViewDetachedFromWindow(this);
  }
  protected void onFinishInflate() {}
  protected void onAttachedToWindow() {}
  protected void onDetachedFromWindow() {}
  public void onWindowFocusChanged(boolean focus) {}
  public boolean dispatchKeyEvent(KeyEvent e) {
    if (keyListener != null && keyListener.onKey(this, e.getKeyCode(), e)) return true;
    // AOSP View.dispatchKeyEvent: an unconsumed listener event reaches the
    // view's key callbacks (TextView delegates those to its editing listener).
    return switch (e.getAction()) {
      case KeyEvent.ACTION_DOWN -> onKeyDown(e.getKeyCode(), e);
      case KeyEvent.ACTION_UP -> onKeyUp(e.getKeyCode(), e);
      default -> false;
    };
  }
  public boolean onKeyDown(int code, KeyEvent event) {
    return false;
  }
  public boolean onKeyUp(int code, KeyEvent event) {
    return false;
  }
  public boolean onTouchEvent(MotionEvent e) {
    return false;
  }
  private OnTouchListener touchListener;
  public void setOnTouchListener(OnTouchListener listener) {
    touchListener = listener;
  }
  public boolean dispatchTouchEvent(MotionEvent event) {
    return (enabled && touchListener != null && touchListener.onTouch(this, event))
        || onTouchEvent(event);
  }
  public boolean onGenericMotionEvent(MotionEvent e) {
    return false;
  }
  private OnApplyWindowInsetsListener insetsListener;
  public void setOnApplyWindowInsetsListener(OnApplyWindowInsetsListener l) {
    insetsListener = l;
  }
  public WindowInsets getRootWindowInsets() {
    return new WindowInsets(new android.graphics.Rect());
  }
  public WindowInsets onApplyWindowInsets(WindowInsets i) {
    return i;
  }
  public WindowInsets dispatchApplyWindowInsets(WindowInsets i) {
    return insetsListener == null ? onApplyWindowInsets(i)
                                  : insetsListener.onApplyWindowInsets(this, i);
  }
  private android.graphics.drawable.Drawable background;
  public void setBackgroundColor(int color) {
    setBackground(new android.graphics.drawable.ColorDrawable(color));
  }
  public void setBackground(android.graphics.drawable.Drawable d) {
    background = d;
  }
  public void setBackgroundDrawable(android.graphics.drawable.Drawable d) {
    setBackground(d);
  }
  public android.graphics.drawable.Drawable getBackground() {
    return background;
  }
  private final WindowInsetsController insetsController = new QuestWindowInsetsController();
  public WindowInsetsController getWindowInsetsController() {
    return insetsController;
  }
  private OnKeyListener keyListener;
  public void setOnKeyListener(OnKeyListener l) {
    keyListener = l;
  }
  public interface OnKeyListener {
    boolean onKey(View v, int keyCode, KeyEvent event);
  }
  private final java.util.List<OnAttachStateChangeListener> attachListeners =
      new java.util.concurrent.CopyOnWriteArrayList<>();
  public void addOnAttachStateChangeListener(OnAttachStateChangeListener l) {
    attachListeners.add(l);
  }
  public void removeOnAttachStateChangeListener(OnAttachStateChangeListener l) {
    attachListeners.remove(l);
  }
  public interface OnAttachStateChangeListener {
    void onViewAttachedToWindow(View v);
    void onViewDetachedFromWindow(View v);
  }
  private static final java.util.concurrent.atomic.AtomicInteger nextId =
      new java.util.concurrent.atomic.AtomicInteger(1);
  public static int generateViewId() {
    for (;;) {
      int id = nextId.get();
      if (nextId.compareAndSet(id, id == 0xffffff ? 1 : id + 1))
        return id;
    }
  }
  private Object tag;
  private final java.util.Map<Integer, Object> tags = new java.util.HashMap<>();
  public void setTag(Object value) {
    tag = value;
  }
  public Object getTag() {
    return tag;
  }
  public void setTag(int key, Object value) {
    if (value == null)
      tags.remove(key);
    else
      tags.put(key, value);
  }
  public Object getTag(int key) {
    return tags.get(key);
  }
  private android.graphics.drawable.Drawable foreground;
  private int paddingLeft, paddingTop, paddingRight, paddingBottom, minWidth, minHeight;
  private boolean fitsSystemWindows;
  public void setForeground(android.graphics.drawable.Drawable d) {
    foreground = d;
  }
  public android.graphics.drawable.Drawable getForeground() {
    return foreground;
  }
  public void setPadding(int l, int t, int r, int b) {
    paddingLeft = l;
    paddingTop = t;
    paddingRight = r;
    paddingBottom = b;
  }
  public int getPaddingLeft() {
    return paddingLeft;
  }
  public int getPaddingTop() {
    return paddingTop;
  }
  public int getPaddingRight() {
    return paddingRight;
  }
  public int getPaddingBottom() {
    return paddingBottom;
  }
  public void setMinimumWidth(int w) {
    minWidth = w;
  }
  public int getMinimumWidth() {
    return minWidth;
  }
  public void setMinimumHeight(int h) {
    minHeight = h;
  }
  public int getMinimumHeight() {
    return minHeight;
  }
  public boolean isLaidOut() {
    return isAttachedToWindow() && width > 0 && height > 0;
  }
  public void setFitsSystemWindows(boolean f) {
    fitsSystemWindows = f;
  }
  public boolean getFitsSystemWindows() {
    return fitsSystemWindows;
  }
  public int getWindowSystemUiVisibility() {
    return getRootView().getSystemUiVisibility();
  }
  public void requestApplyInsets() {
    if (attached)
      post(() -> dispatchApplyWindowInsets(getRootWindowInsets()));
  }
  public void requestFitSystemWindows() {
    requestApplyInsets();
  }
  public void postOnAnimation(Runnable r) {
    postOnAnimationDelayed(r, 0);
  }
  public void postOnAnimationDelayed(Runnable r, long delay) {
    long period =
        Math.max(1, Math.round(1000f / com.venture.runtime.RuntimeEnvironment.get().refreshRate));
    long now = android.os.SystemClock.uptimeMillis();
    getHandler().postAtTime(r, ((now + Math.max(0, delay)) / period + 1) * period);
  }
  public interface OnApplyWindowInsetsListener {
    WindowInsets onApplyWindowInsets(View v, WindowInsets i);
  }
  public interface OnSystemUiVisibilityChangeListener {
    void onSystemUiVisibilityChange(int visibility);
  }
  public interface OnFocusChangeListener {
    void onFocusChange(View v, boolean focus);
  }
  public interface OnClickListener {
    void onClick(View v);
  }
  public interface OnCreateContextMenuListener {
    void onCreateContextMenu(ContextMenu menu, View view, ContextMenu.ContextMenuInfo info);
  }
  private OnCreateContextMenuListener contextMenuListener;
  public void setOnCreateContextMenuListener(OnCreateContextMenuListener listener) {
    contextMenuListener = listener;
  }
  public void createContextMenu(ContextMenu menu) {
    if (contextMenuListener != null)
      contextMenuListener.onCreateContextMenu(menu, this, null);
  }
  public static class AccessibilityDelegate {
    public boolean dispatchPopulateAccessibilityEvent(
        View host, android.view.accessibility.AccessibilityEvent event) {
      return false;
    }
    public void onInitializeAccessibilityEvent(
        View host, android.view.accessibility.AccessibilityEvent event) {
      event.setClassName(host.getClass().getName());
      event.setContentDescription(host.getContentDescription());
    }
    public void onPopulateAccessibilityEvent(
        View host, android.view.accessibility.AccessibilityEvent event) {}
    public boolean onRequestSendAccessibilityEvent(
        ViewGroup host, View child, android.view.accessibility.AccessibilityEvent event) {
      return true;
    }
    public void sendAccessibilityEvent(View host, int type) {
      sendAccessibilityEventUnchecked(
          host, android.view.accessibility.AccessibilityEvent.obtain(type));
    }
    public void sendAccessibilityEventUnchecked(
        View host, android.view.accessibility.AccessibilityEvent event) {
      // AccessibilityManager reports disabled until a host accessibility
      // service exists.
      com.venture.runtime.RuntimeEnvironment.trace(
          "Accessibility event: no enabled host service");
    }
    public boolean performAccessibilityAction(View host, int action, android.os.Bundle arguments) {
      return false;
    }
  }
  private AccessibilityDelegate accessibilityDelegate;
  public void setAccessibilityDelegate(AccessibilityDelegate delegate) {
    accessibilityDelegate = delegate;
  }
  public AccessibilityDelegate getAccessibilityDelegate() {
    return accessibilityDelegate;
  }
  private OnClickListener clickListener;
  private boolean clickable, longClickable;
  public void setOnClickListener(OnClickListener listener) {
    clickListener = listener;
    if (!clickable) setClickable(true);
  }
  public void setClickable(boolean clickable) { this.clickable = clickable; }
  public boolean isClickable() { return clickable; }
  public void setLongClickable(boolean longClickable) { this.longClickable = longClickable; }
  public boolean isLongClickable() { return longClickable; }
  public void bringToFront() { if (parent instanceof ViewGroup group) group.bringChildToFront(this); }
  public boolean onKeyPreIme(int keyCode, KeyEvent event) { return false; }
  public boolean hasOnClickListeners() {
    return clickListener != null;
  }
  public boolean performClick() {
    if (clickListener == null)
      return false;
    clickListener.onClick(this);
    return true;
  }
  public interface OnTouchListener {
    boolean onTouch(View v, MotionEvent event);
  }
  public final void cancelPendingInputEvents() {
    dispatchCancelPendingInputEvents();
  }
  void dispatchCancelPendingInputEvents() {
    onCancelPendingInputEvents();
  }
  protected void onCancelPendingInputEvents() {
    // Base View dispatches input/clicks synchronously. Subclasses with queued
    // input work override this callback; arbitrary posted tasks are not input.
    com.venture.runtime.RuntimeEnvironment.trace("View.cancelPendingInputEvents");
  }
}
