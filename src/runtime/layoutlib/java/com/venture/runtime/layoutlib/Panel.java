package com.venture.runtime.layoutlib;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.PorterDuff;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.VentureWindow;
import android.view.View;
import android.view.WindowManager;
import com.venture.runtime.RuntimeEnvironment;
import java.io.File;
import java.io.FileOutputStream;
import java.util.ArrayList;
import java.util.List;

/**
 * The 2D app's panel: the windows it adds, stacked as a window manager stacks
 * them, composited into one image the host shows (a desktop window, or a quad
 * in VR), and the pointer and key input the host sends back, routed to windows
 * as Android's input dispatcher routes it. Main thread only.
 */
public final class Panel {
  private static Panel instance;
  // Virtual displays' panels by display ID (DisplayManager.createVirtualDisplay).
  private static final java.util.Map<Integer, Panel> displays = new java.util.concurrent.ConcurrentHashMap<>();

  public static synchronized Panel get() {
    if (instance == null) instance = new Panel(Layoutlib.display(), Layoutlib.PANEL_WIDTH, Layoutlib.PANEL_HEIGHT, null);
    return instance;
  }

  /**
   * A virtual display's panel: the windows on that display (a Presentation),
   * composited into the display's Surface instead of the host's window.
   */
  public static Panel createForDisplay(android.view.Display display, int width, int height, android.view.Surface surface) {
    Panel panel = new Panel(display, width, height, surface);
    displays.put(display.getDisplayId(), panel);
    return panel;
  }

  /** The panel showing a display: the 2D panel for the default display, else a virtual display's, or null. */
  public static Panel forDisplay(int displayId) {
    return displayId == android.view.Display.DEFAULT_DISPLAY ? get() : displays.get(displayId);
  }

  /** A released virtual display's windows go with it, as the window manager removes them. */
  public static void releaseDisplay(int displayId) {
    Panel panel = displays.remove(displayId);
    if (panel == null) return;
    panel.main.post(() -> {
      for (VentureWindow window : new ArrayList<>(panel.windows)) window.remove();
      panel.output = null;
    });
  }

  // Activities between stop and restart or destroy. Their windows, on every
  // panel, are stopped: they neither draw nor composite, so a stopped app's main
  // thread goes quiet as on a device.
  private static final java.util.Set<android.app.Activity> stoppedActivities =
      java.util.Collections.newSetFromMap(new java.util.IdentityHashMap<>());
  // Resumed activities. Window focus belongs to the foreground app: once none
  // of this process's activities is resumed, none of its windows has focus,
  // on any display (a focused text field stops its cursor blink, as when
  // another task comes to the front).
  private static final java.util.Set<android.app.Activity> resumedActivities =
      java.util.Collections.newSetFromMap(new java.util.IdentityHashMap<>());

  /** Activity resume and pause: whether this process holds window focus. */
  public static void activityResumed(android.app.Activity activity, boolean resumed) {
    if (resumed ? !resumedActivities.add(activity) : !resumedActivities.remove(activity)) return;
    for (Panel panel : all()) panel.updateFocus();
  }

  private static List<Panel> all() {
    List<Panel> panels = new ArrayList<>();
    synchronized (Panel.class) {
      if (instance != null) panels.add(instance);
    }
    panels.addAll(displays.values());
    return panels;
  }

  /**
   * Whether a window is the activity's: its token is the activity's (the
   * activity's own window and its dialogs'), it is a sub-window of one of the
   * activity's windows (a popup names its parent's token), or the activity's
   * context added it (a presentation, whose window has a token of its own).
   */
  private static boolean belongsTo(VentureWindow window, android.app.Activity activity) {
    for (int depth = 0; window != null && depth < 8; ++depth) {
      if (window.owner() == activity) return true;
      android.os.IBinder token = window.params().token;
      if (token == null) return false;
      if (token == activity.getActivityToken()) return true;
      VentureWindow parent = null;
      for (Panel panel : all())
        for (VentureWindow candidate : panel.windows)
          if (candidate != window && candidate.windowToken() == token) parent = candidate;
      window = parent;
    }
    return false;
  }

  /** Activity stop and restart: WindowManagerGlobal.setStoppedState for the windows the activity added. */
  public static void activityStopped(android.app.Activity activity, boolean stopped) {
    if (stopped ? !stoppedActivities.add(activity) : !stoppedActivities.remove(activity)) return;
    for (Panel panel : all())
      for (VentureWindow window : new ArrayList<>(panel.windows))
        if (belongsTo(window, activity)) window.setStopped(stopped);
  }

  /**
   * A destroyed activity's remaining windows are removed, as
   * WindowManagerGlobal.closeAll removes the windows an activity leaks.
   */
  public static void activityDestroyed(android.app.Activity activity) {
    stoppedActivities.remove(activity);
    activityResumed(activity, false);
    for (Panel panel : all()) {
      for (VentureWindow window : new ArrayList<>(panel.windows)) {
        if (window.removed() || !belongsTo(window, activity)) continue;
        android.util.Log.w("WindowManager", "Activity " + activity.getComponentName().getClassName()
            + " has leaked window " + window.params().getTitle() + " that was originally added here");
        window.remove();
      }
    }
  }

  private final android.view.Display display;
  private int width, height;
  private final List<VentureWindow> windows = new ArrayList<>();
  private Bitmap image;
  private Canvas canvas;
  private android.view.Surface output;
  private int[] outputPixels;
  private final Handler main = new Handler(Looper.getMainLooper());
  private boolean compositeScheduled;
  private VentureWindow pointerTarget;
  private long frames;
  private final String dump = System.getenv("VENTURE_PANEL_DUMP");
  private long lastDump;

  private Panel(android.view.Display display, int width, int height, android.view.Surface output) {
    this.display = display;
    this.output = output;
    resize(width, height);
  }

  private void resize(int width, int height) {
    this.width = width;
    this.height = height;
    image = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
    canvas = new Canvas(image);
    outputPixels = null;
  }

  /** A virtual display's new Surface or size (VirtualDisplay.setSurface, resize). */
  public void output(android.view.Surface surface, int width, int height) {
    main.post(() -> {
      output = surface;
      if (width != this.width || height != this.height) {
        resize(width, height);
        for (VentureWindow window : windows) window.view().requestLayout();
      }
      scheduleComposite();
    });
  }

  public android.view.Display display() { return display; }
  public boolean isDefault() { return display.getDisplayId() == android.view.Display.DEFAULT_DISPLAY; }
  public int width() { return width; }
  public int height() { return height; }
  public Bitmap image() { return image; }
  public long frames() { return frames; }

  private static int layer(WindowManager.LayoutParams lp) {
    return lp.type >= WindowManager.LayoutParams.FIRST_SYSTEM_WINDOW ? 2 : 1;
  }

  public void addWindow(View view, WindowManager.LayoutParams lp, Context context) {
    VentureWindow window = new VentureWindow(context, display, this);
    int index = windows.size();
    while (index > 0 && layer(windows.get(index - 1).params()) > layer(lp)) --index;
    windows.add(index, window);
    try {
      window.attach(view, lp, null);
    } catch (RuntimeException | Error failure) {
      windows.remove(window);
      throw failure;
    }
    for (android.app.Activity activity : stoppedActivities)
      if (belongsTo(window, activity)) window.setStopped(true);
    updateFocus();
  }

  private VentureWindow find(View view) {
    for (VentureWindow window : windows) if (window.view() == view) return window;
    return null;
  }

  public void updateWindow(View view, WindowManager.LayoutParams lp) {
    VentureWindow window = find(view);
    if (window == null) throw new IllegalArgumentException("View not attached to window manager");
    window.params().copyFrom(lp);
    view.requestLayout();
    updateFocus();
  }

  public void removeWindow(View view) {
    VentureWindow window = find(view);
    if (window == null) throw new IllegalArgumentException("View not attached to window manager");
    window.remove();
  }

  public void windowRemoved(VentureWindow window) {
    windows.remove(window);
    if (pointerTarget == window) pointerTarget = null;
    updateFocus();
    scheduleComposite();
  }

  public void windowDrawn(VentureWindow window) { scheduleComposite(); }

  /** A window stopped or restarted: it leaves or rejoins the composite and focus. */
  public void windowStopped(VentureWindow window) {
    if (window.stopped() && pointerTarget == window) pointerTarget = null;
    updateFocus();
    scheduleComposite();
  }

  // SurfaceViews with a Surface on this panel's windows. Their producers post
  // on their own threads; the panel takes each new frame once per refresh.
  private final List<android.view.SurfaceView> surfaces = new ArrayList<>();
  private boolean polling;
  private final int[] location = new int[2];
  private final android.graphics.Paint surfacePaint = new android.graphics.Paint(android.graphics.Paint.FILTER_BITMAP_FLAG);

  public void addSurface(android.view.SurfaceView view) {
    if (!surfaces.contains(view)) surfaces.add(view);
    idlePolls = 0;
    poll();
  }

  public void removeSurface(android.view.SurfaceView view) {
    if (surfaces.remove(view)) scheduleComposite();
  }

  // Refreshes without a new frame. Once the producers have been idle for about a
  // second (a Surface nothing draws to, a paused game) the panel looks every two
  // seconds instead, so an idle app's main thread stays quiet.
  private int idlePolls;
  private static final int IDLE_POLLS = 60, IDLE_INTERVAL_MS = 2000;

  private void poll() {
    if (polling || surfaces.isEmpty()) return;
    polling = true;
    float refresh = display.getRefreshRate();
    int interval = Math.max(1, Math.round(1000f / (refresh > 0 ? refresh : 60f)));
    main.postDelayed(this::pollSurfaces, idlePolls >= IDLE_POLLS ? IDLE_INTERVAL_MS : interval);
  }

  private void pollSurfaces() {
    polling = false;
    boolean changed = false;
    for (android.view.SurfaceView view : new ArrayList<>(surfaces)) changed |= view.ventureFetchFrame();
    if (changed) {
      idlePolls = 0;
      scheduleComposite();
    } else if (idlePolls < IDLE_POLLS) {
      ++idlePolls;
    }
    poll();
  }

  private void drawSurfaces(VentureWindow window, android.graphics.Rect bounds, boolean top) {
    for (android.view.SurfaceView view : surfaces) {
      if (view.isZOrderedOnTop() != top || view.getRootView() != window.view()) continue;
      Bitmap frame = view.ventureFrame();
      if (frame == null) continue;
      view.getLocationInWindow(location);
      int left = bounds.left + location[0], top0 = bounds.top + location[1];
      canvas.drawBitmap(frame, null,
          new android.graphics.Rect(left, top0, left + view.getWidth(), top0 + view.getHeight()), surfacePaint);
    }
  }

  private void updateFocus() {
    VentureWindow focused = focusedWindow();
    for (VentureWindow window : windows) window.focus(window == focused);
  }

  private VentureWindow focusedWindow() {
    if (resumedActivities.isEmpty()) return null;
    for (int i = windows.size() - 1; i >= 0; --i) {
      VentureWindow window = windows.get(i);
      if (!window.stopped() && (window.params().flags & WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE) == 0)
        return window;
    }
    return null;
  }

  private void scheduleComposite() {
    if (compositeScheduled) return;
    compositeScheduled = true;
    main.post(this::composite);
  }

  private void composite() {
    compositeScheduled = false;
    // A virtual display starts each frame transparent, as SurfaceFlinger
    // clears one: its consumer (an XR quad) blends what the windows leave clear.
    canvas.drawColor(isDefault() ? Color.BLACK : Color.TRANSPARENT, PorterDuff.Mode.SRC);
    for (VentureWindow window : windows) {
      Bitmap frame = window.frame();
      if (frame == null || window.stopped()) continue;
      WindowManager.LayoutParams lp = window.params();
      if ((lp.flags & WindowManager.LayoutParams.FLAG_DIM_BEHIND) != 0)
        canvas.drawColor(Color.argb(Math.round(lp.dimAmount * 255), 0, 0, 0));
      android.graphics.Rect bounds = window.bounds();
      drawSurfaces(window, bounds, false);
      canvas.drawBitmap(frame, new android.graphics.Rect(0, 0, bounds.width(), bounds.height()), bounds, null);
      drawSurfaces(window, bounds, true);
    }
    ++frames;
    if (isDefault()) {
      if (frames == 1) PanelScript.start(this);
      PanelHost.present(this);
    } else {
      publish();
    }
    if (dump != null) {
      long now = SystemClock.uptimeMillis();
      if (now - lastDump >= 1000) {
        lastDump = now;
        String name = isDefault() ? "panel" : "display" + display.getDisplayId();
        File out = new File(dump, String.format("%s-%06d.png", name, frames));
        try (FileOutputStream stream = new FileOutputStream(out)) {
          image.compress(Bitmap.CompressFormat.PNG, 100, stream);
        } catch (java.io.IOException e) {
          android.util.Log.w("Panel", "Cannot write " + out + ": " + e);
        }
        File tree = new File(dump, String.format("%s-%06d.txt", name, frames));
        try (java.io.PrintWriter writer = new java.io.PrintWriter(tree, "UTF-8")) {
          for (VentureWindow window : windows) {
            writer.println("window " + window.params().getTitle() + " " + window.bounds().toShortString());
            if (window.view() != null) describe(writer, window.view(), 1);
          }
        } catch (java.io.IOException e) {
          android.util.Log.w("Panel", "Cannot write " + tree + ": " + e);
        }
      }
    }
  }

  // A virtual display's frame goes to its Surface, as SurfaceFlinger composites
  // a virtual display into the BufferQueue its creator handed over. A display
  // without a Surface is paused.
  private void publish() {
    android.view.Surface surface = output;
    if (surface == null || surface.ventureNativeWindow() == 0) return;
    if (outputPixels == null) outputPixels = new int[width * height];
    image.getPixels(outputPixels, 0, width, 0, 0, width, height);
    int result = com.venture.runtime.NativeHost.publishSurfacePixels(surface.ventureNativeWindow(), width, height,
        outputPixels);
    if (result < 0 && frames == 1)
      android.util.Log.w("Panel", "Display " + display.getDisplayId() + " cannot post to its Surface: " + result);
  }

  // Pointer input, in panel pixels. Android's input dispatcher sends a gesture to
  // the window it went down in: the topmost touch-modal window, else the topmost
  // window under it; windows that watch outside touches get ACTION_OUTSIDE.
  public void pointer(int action, float x, float y, long downTime, long eventTime, int source, int buttons) {
    int masked = action & MotionEvent.ACTION_MASK;
    boolean hover = masked == MotionEvent.ACTION_HOVER_MOVE || masked == MotionEvent.ACTION_HOVER_ENTER
        || masked == MotionEvent.ACTION_HOVER_EXIT || masked == MotionEvent.ACTION_SCROLL;
    VentureWindow target = pointerTarget;
    if (masked == MotionEvent.ACTION_DOWN || hover || target == null || target.removed()) {
      target = null;
      for (int i = windows.size() - 1; i >= 0; --i) {
        VentureWindow window = windows.get(i);
        int flags = window.params().flags;
        if (window.stopped() || (flags & WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE) != 0) continue;
        boolean inside = window.bounds().contains((int) x, (int) y);
        boolean modal = (flags & (WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
            | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL)) == 0;
        if (inside || modal) { target = window; break; }
        if (masked == MotionEvent.ACTION_DOWN && (flags & WindowManager.LayoutParams.FLAG_WATCH_OUTSIDE_TOUCH) != 0) {
          MotionEvent outside = MotionEvent.obtain(downTime, eventTime, MotionEvent.ACTION_OUTSIDE, 0, 0, 0);
          outside.setSource(source);
          window.deliver(outside);
        }
      }
      if (!hover) pointerTarget = target;
    }
    if (target == null) return;
    MotionEvent event = MotionEvent.obtain(downTime, eventTime, action, x - target.bounds().left,
        y - target.bounds().top, 0);
    event.setSource(source);
    if (buttons != 0) event.setButtonState(buttons);
    target.deliver(event);
    if (masked == MotionEvent.ACTION_UP || masked == MotionEvent.ACTION_CANCEL) pointerTarget = null;
  }

  public void scroll(float x, float y, float horizontal, float vertical, long eventTime) {
    MotionEvent.PointerProperties properties = new MotionEvent.PointerProperties();
    properties.id = 0;
    properties.toolType = MotionEvent.TOOL_TYPE_MOUSE;
    MotionEvent.PointerCoords coords = new MotionEvent.PointerCoords();
    coords.x = x;
    coords.y = y;
    coords.setAxisValue(MotionEvent.AXIS_HSCROLL, horizontal);
    coords.setAxisValue(MotionEvent.AXIS_VSCROLL, vertical);
    for (int i = windows.size() - 1; i >= 0; --i) {
      VentureWindow window = windows.get(i);
      if (window.stopped() || !window.bounds().contains((int) x, (int) y)) continue;
      coords.x = x - window.bounds().left;
      coords.y = y - window.bounds().top;
      MotionEvent event = MotionEvent.obtain(eventTime, eventTime, MotionEvent.ACTION_SCROLL, 1,
          new MotionEvent.PointerProperties[] {properties}, new MotionEvent.PointerCoords[] {coords}, 0, 0, 1f, 1f,
          0, 0, InputDevice.SOURCE_MOUSE, 0);
      window.deliver(event);
      return;
    }
  }

  public void key(KeyEvent event) {
    VentureWindow window = focusedWindow();
    if (window != null) window.deliver(event);
  }

  /** One line per view beside a panel dump: class, id, bounds in its parent, state, text. */
  private static void describe(java.io.PrintWriter writer, View view, int depth) {
    StringBuilder line = new StringBuilder("  ".repeat(depth)).append(view.getClass().getName());
    if (view.getId() != View.NO_ID) {
      try {
        line.append(" #").append(view.getResources().getResourceEntryName(view.getId()));
      } catch (RuntimeException unnamed) {
        line.append(" #").append(Integer.toHexString(view.getId()));
      }
    }
    line.append(' ').append(view.getLeft()).append(',').append(view.getTop()).append(' ')
        .append(view.getWidth()).append('x').append(view.getHeight());
    if (view.getVisibility() != View.VISIBLE) line.append(view.getVisibility() == View.GONE ? " gone" : " invisible");
    if (view.getAlpha() != 1f) line.append(" alpha=").append(view.getAlpha());
    if (view.getTranslationX() != 0f || view.getTranslationY() != 0f)
      line.append(" translate=").append(view.getTranslationX()).append(',').append(view.getTranslationY());
    if (view instanceof android.widget.TextView text && text.getText() != null) {
      line.append(" \"").append(text.getText().toString().replace('\n', ' ')).append("\" color=#")
          .append(Integer.toHexString(text.getCurrentTextColor()));
    }
    writer.println(line);
    if (view instanceof android.view.ViewGroup group)
      for (int i = 0; i < group.getChildCount(); ++i) describe(writer, group.getChildAt(i), depth + 1);
  }

  /** The panel's windows, topmost last (diagnostics and tests). */
  public List<VentureWindow> windows() { return windows; }

  static void trace(String message) { RuntimeEnvironment.trace("Panel: " + message); }
}
