package android.view;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Rect;
import android.os.SystemClock;
import com.venture.runtime.layoutlib.Panel;
import java.nio.ByteBuffer;

/**
 * One window of a 2D process (an activity, a dialog, a popup) on Venture's
 * panel. It is the window's ViewRootImpl: setView, input dispatch, invalidation
 * and focus are AOSP's. The traversal is Venture's, as in layoutlib's render
 * sessions: there is no window manager to size the window and no SurfaceFlinger
 * to give it a surface, so it measures and lays the view out on the panel and
 * HWUI draws it into a buffer (layoutlib's LayoutlibRenderer), which the panel
 * composites and presents (com.venture.runtime.layoutlib.Panel).
 */
public final class VentureWindow extends ViewRootImpl {
  private final Panel panel;
  private ThreadedRenderer threaded;
  private LayoutlibRenderer renderer;
  private int rendererWidth, rendererHeight;
  private Bitmap frame;
  private int frameWidth;
  private boolean attached, removed, stopped, dirty = true;
  private final android.app.Activity owner;
  private final Rect bounds = new Rect();
  private final Choreographer.FrameCallback traversal = frameTime -> traverse();

  public VentureWindow(Context context, Display display, Panel panel) {
    this(context, display, panel, new Session());
  }

  private VentureWindow(Context context, Display display, Panel panel, Session session) {
    super(context, display, session, new WindowLayout());
    this.panel = panel;
    this.owner = activityOf(context);
    session.window = this;
  }

  /**
   * The window manager's session as this window sees it. Only the window ID is
   * answered: View.getWindowId asks the session for it, and AOSP and AndroidX
   * transitions compare the IDs of the windows views are in.
   */
  private static final class Session extends IWindowSession.Default {
    VentureWindow window;
    private final java.util.List<IWindowFocusObserver> observers = new java.util.concurrent.CopyOnWriteArrayList<>();
    private final IWindowId id = new IWindowId.Stub() {
      @Override public void registerFocusObserver(IWindowFocusObserver observer) { observers.add(observer); }
      @Override public void unregisterFocusObserver(IWindowFocusObserver observer) { observers.remove(observer); }
      @Override public boolean isFocused() { return window != null && window.mAttachInfo.mHasWindowFocus; }
    };

    @Override public IWindowId getWindowId(android.os.IBinder window) { return id; }

    void focusChanged(boolean focused, android.os.IBinder token) {
      for (IWindowFocusObserver observer : observers) {
        try {
          if (focused) observer.focusGained(token);
          else observer.focusLost(token);
        } catch (android.os.RemoteException unreachable) {
          // In-process observers do not throw it.
        }
      }
    }
  }

  public void attach(View view, WindowManager.LayoutParams params, View panelParentView) {
    setView(view, params, panelParentView);
  }

  public WindowManager.LayoutParams params() { return mWindowAttributes; }
  public View view() { return mView; }
  public Rect bounds() { return bounds; }
  public Bitmap frame() { return frame; }
  public boolean removed() { return removed; }

  /** The activity whose context added this window (a presentation's), or null. */
  public android.app.Activity owner() { return owner; }
  /** The window's own token, which its sub-windows (popups, panels) name as theirs. */
  public android.os.IBinder windowToken() { return mAttachInfo.mWindowToken; }
  public boolean stopped() { return stopped; }

  private static android.app.Activity activityOf(Context context) {
    for (int depth = 0; context != null && depth < 32; ++depth) {
      if (context instanceof android.app.Activity activity) return activity;
      if (!(context instanceof android.content.ContextWrapper wrapper)) return null;
      context = wrapper.getBaseContext();
    }
    return null;
  }

  /**
   * The window manager stops the windows of an activity that is no longer
   * visible (WindowManagerGlobal.setStoppedState): the window is hidden from its
   * views (a SurfaceView gives up its Surface, a text cursor stops blinking) and
   * draws nothing until its activity restarts.
   */
  public void setStopped(boolean stopped) {
    if (this.stopped == stopped || removed) return;
    this.stopped = stopped;
    if (stopped) unscheduleTraversals();
    View host = mView;
    if (attached && host != null) {
      int visibility = stopped ? View.GONE : View.VISIBLE;
      mAttachInfo.mWindowVisibility = visibility;
      host.dispatchWindowVisibilityChanged(visibility);
      host.dispatchVisibilityAggregated(!stopped && host.getVisibility() == View.VISIBLE);
    }
    if (!stopped) scheduleTraversals();
    panel.windowStopped(this);
  }

  @Override
  void scheduleTraversals() {
    dirty = true;
    if (!mTraversalScheduled && !removed && !stopped) {
      mTraversalScheduled = true;
      mChoreographer.postFrameCallback(traversal);
    }
  }

  @Override
  void unscheduleTraversals() {
    if (mTraversalScheduled) {
      mTraversalScheduled = false;
      mChoreographer.removeFrameCallback(traversal);
    }
  }

  /** A traversal now, outside the frame clock (the panel's first frame, a resize). */
  public void traverseNow() {
    unscheduleTraversals();
    traverse();
  }

  private static int rootMeasureSpec(int windowSize, int measurement) {
    if (measurement == ViewGroup.LayoutParams.MATCH_PARENT)
      return View.MeasureSpec.makeMeasureSpec(windowSize, View.MeasureSpec.EXACTLY);
    if (measurement == ViewGroup.LayoutParams.WRAP_CONTENT)
      return View.MeasureSpec.makeMeasureSpec(windowSize, View.MeasureSpec.AT_MOST);
    return View.MeasureSpec.makeMeasureSpec(measurement, View.MeasureSpec.EXACTLY);
  }

  private void traverse() {
    mTraversalScheduled = false;
    View host = mView;
    if (host == null || removed || stopped) return;
    int panelWidth = panel.width(), panelHeight = panel.height();
    WindowManager.LayoutParams lp = mWindowAttributes;
    if (!attached) {
      attached = true;
      if (mAttachInfo.mThreadedRenderer != null) mAttachInfo.mThreadedRenderer.destroy();
      threaded = ThreadedRenderer.create(mContext, true, String.valueOf(lp.getTitle()));
      renderer = new LayoutlibRenderer(threaded);
      mAttachInfo.mThreadedRenderer = threaded;
      mAttachInfo.mHardwareAccelerated = true;
      mAttachInfo.mWindowVisibility = View.VISIBLE;
      mAttachInfo.mApplicationScale = 1f;
      mTmpFrames.displayFrame.set(0, 0, panelWidth, panelHeight);
      mFirst = false;
      host.dispatchAttachedToWindow(mAttachInfo, 0);
      mAttachInfo.mTreeObserver.dispatchOnWindowAttachedChange(true);
      dispatchApplyInsets(host);
    }
    boolean layout = host.isLayoutRequested() || bounds.isEmpty() || host.getWidth() == 0;
    if (layout) {
      // ViewRootImpl.measureHierarchy: a wrap-content window first tries the
      // preferred dialog width.
      int widthSpec = rootMeasureSpec(panelWidth, lp.width);
      if (lp.width == ViewGroup.LayoutParams.WRAP_CONTENT) {
        android.util.TypedValue value = new android.util.TypedValue();
        mContext.getResources().getValue(com.android.internal.R.dimen.config_prefDialogWidth, value, true);
        int preferred = value.type == android.util.TypedValue.TYPE_DIMENSION
            ? (int) value.getDimension(mContext.getResources().getDisplayMetrics()) : 0;
        if (preferred > 0 && preferred < panelWidth) {
          host.measure(rootMeasureSpec(preferred, lp.width), rootMeasureSpec(panelHeight, lp.height));
          if ((host.getMeasuredWidthAndState() & View.MEASURED_STATE_TOO_SMALL) == 0)
            widthSpec = rootMeasureSpec(preferred, lp.width);
        }
      }
      host.measure(widthSpec, rootMeasureSpec(panelHeight, lp.height));
      int width = host.getMeasuredWidth(), height = host.getMeasuredHeight();
      Rect frame = new Rect();
      Gravity.apply(lp.gravity == 0 ? Gravity.TOP | Gravity.START : lp.gravity, width, height,
          new Rect(0, 0, panelWidth, panelHeight), lp.x, lp.y, frame, host.getLayoutDirection());
      bounds.set(frame);
      mWinFrame.set(frame);
      mTmpFrames.frame.set(frame);
      mWidth = width;
      mHeight = height;
      mAttachInfo.mWindowLeft = frame.left;
      mAttachInfo.mWindowTop = frame.top;
      host.layout(0, 0, width, height);
      mAttachInfo.mTreeObserver.dispatchOnGlobalLayout();
    }
    int width = Math.max(1, bounds.width()), height = Math.max(1, bounds.height());
    if (renderer == null) return;
    if (width != rendererWidth || height != rendererHeight) {
      rendererWidth = width;
      rendererHeight = height;
      renderer.setup(width, height, host);
      frame = null;
    }
    if (mAttachInfo.mTreeObserver.dispatchOnPreDraw()) {
      scheduleTraversals();  // a listener asked to redraw first
      return;
    }
    mAttachInfo.mDrawingTime = SystemClock.uptimeMillis();
    renderer.draw(java.util.List.of(host));
    ByteBuffer pixels = renderer.getBuffer();
    try {
      // Rows may be padded: the frame bitmap is as wide as a row, and the
      // panel draws its window-sized part.
      int rowPixels = renderer.getRowStride() / 4;
      if (frame == null || frameWidth != rowPixels || frame.getHeight() != height) {
        frameWidth = rowPixels;
        frame = Bitmap.createBitmap(rowPixels, height, Bitmap.Config.ARGB_8888);
      }
      pixels.rewind();
      if (pixels.remaining() >= frame.getByteCount()) frame.copyPixelsFromBuffer(pixels);
    } finally {
      renderer.releaseBuffer();
    }
    dirty = false;
    panel.windowDrawn(this);
  }

  public boolean dirty() { return dirty; }

  /** Window focus, as the window manager reports it. */
  public void focus(boolean focused) {
    if (mAttachInfo.mHasWindowFocus == focused) return;
    windowFocusChanged(focused);
    ((Session) mWindowSession).focusChanged(focused, mAttachInfo.mWindowToken);
  }

  public void remove() {
    if (removed) return;
    removed = true;
    unscheduleTraversals();
    die(true);
    if (renderer != null) renderer.destroy();
    renderer = null;
    threaded = null;
    panel.windowRemoved(this);
  }

  /** Delivers an input event to this window, its coordinates already in the window's space. */
  public void deliver(InputEvent event) {
    dispatchInputEvent(event);
  }
}
