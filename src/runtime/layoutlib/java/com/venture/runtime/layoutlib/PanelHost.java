package com.venture.runtime.layoutlib;

import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.MotionEvent;
import java.nio.ByteBuffer;

/**
 * The host side of the 2D panel: Venture's runtime shows the panel's image in
 * a desktop window (src/runtime/xr/panel_window.h) and sends its pointer and
 * key input back, which this delivers to the panel on the main thread.
 */
public final class PanelHost {
  private PanelHost() {}

  // Event records from the host, six values each (panel_window.h).
  private static final int POINTER = 0, SCROLL = 1, KEY = 2, TEXT = 3, CLOSE = 4;

  private static boolean nativeHost = true;
  private static boolean started;
  private static ByteBuffer pixels;
  private static long pointerDown;
  private static final long[] keyDown = new long[512];

  static void present(Panel panel) {
    if (!nativeHost) return;
    try {
      android.graphics.Bitmap image = panel.image();
      int size = image.getByteCount();
      if (pixels == null || pixels.capacity() != size) pixels = ByteBuffer.allocateDirect(size);
      pixels.rewind();
      image.copyPixelsToBuffer(pixels);
      if (!started) {
        started = true;
        nativeTitle(title());
        startInput(panel);
      }
      nativePresent(panel.width(), panel.height(), pixels);
    } catch (UnsatisfiedLinkError notInThisHost) {
      nativeHost = false;
    }
  }

  private static String title() {
    try {
      android.content.pm.ApplicationInfo info = com.venture.runtime.RuntimeEnvironment.get().manifest.info.applicationInfo;
      CharSequence label = info.labelRes != 0 ? com.venture.runtime.RuntimeEnvironment.resources().getText(info.labelRes)
                                              : info.nonLocalizedLabel;
      if (label != null && label.length() > 0) return label.toString();
    } catch (RuntimeException | LinkageError unlabelled) {
      // the package name below
    }
    return com.venture.runtime.RuntimeEnvironment.get().manifest.info.packageName;
  }

  /** A daemon thread waits for the host's input and hands each batch to the main thread. */
  private static void startInput(Panel panel) {
    Handler main = new Handler(Looper.getMainLooper());
    Thread thread = new Thread(() -> {
      while (true) {
        int[] events = nativeEvents();
        if (events != null) main.post(() -> dispatch(panel, events));
      }
    }, "Venture panel input");
    thread.setDaemon(true);
    thread.start();
  }

  static void dispatch(Panel panel, int[] events) {
    for (int i = 0; i + 5 < events.length; i += 6) {
      long now = SystemClock.uptimeMillis();
      switch (events[i]) {
        case POINTER -> {
          int action = events[i + 1];
          if (action == MotionEvent.ACTION_DOWN) pointerDown = now;
          boolean hover = action == MotionEvent.ACTION_HOVER_MOVE || action == MotionEvent.ACTION_HOVER_EXIT;
          panel.pointer(action, Float.intBitsToFloat(events[i + 2]), Float.intBitsToFloat(events[i + 3]),
              hover ? now : pointerDown, now, events[i + 5], events[i + 4]);
        }
        case SCROLL -> panel.scroll(Float.intBitsToFloat(events[i + 1]), Float.intBitsToFloat(events[i + 2]),
            Float.intBitsToFloat(events[i + 3]), Float.intBitsToFloat(events[i + 4]), now);
        case KEY -> {
          int action = events[i + 1], code = events[i + 2];
          int slot = code & (keyDown.length - 1);
          if (action == KeyEvent.ACTION_DOWN && events[i + 4] == 0) keyDown[slot] = now;
          long down = keyDown[slot] != 0 ? keyDown[slot] : now;
          KeyEvent event = new KeyEvent(down, now, action, code, events[i + 4], events[i + 3],
              KeyCharacterMap.VIRTUAL_KEYBOARD, 0, 0, InputDevice.SOURCE_KEYBOARD);
          panel.key(event);
        }
        case TEXT -> {
          // A typed character, as Android delivers text without a key: one
          // ACTION_MULTIPLE event whose characters the focused view inserts.
          String characters = new String(Character.toChars(events[i + 1]));
          panel.key(new KeyEvent(now, characters, KeyCharacterMap.VIRTUAL_KEYBOARD, 0));
        }
        case CLOSE -> com.venture.runtime.AppMain.requestShutdown();
        default -> {}
      }
    }
  }

  /** Document picker kinds, as the host's dialog takes them (panel_window.cpp). */
  public static final int PICK_FOLDER = 0, PICK_OPEN = 1, PICK_SAVE = 2;

  /**
   * The host's file or folder dialog: the chosen host paths, or null when the
   * person cancelled or the host has no window to show it. Blocks; never call
   * it on the main thread. A development script's answer comes first.
   */
  public static String[] pick(int kind, String name, boolean many) {
    String scripted = PanelScript.takePick();
    if (scripted != null) return scripted.isEmpty() ? null : new String[] {scripted};
    if (!nativeHost) return null;
    try {
      return nativePick(kind, name, many);
    } catch (UnsatisfiedLinkError notInThisHost) {
      return null;
    }
  }

  private static native String[] nativePick(int kind, String name, boolean many);

  private static native void nativePresent(int width, int height, ByteBuffer pixels);

  private static native void nativeTitle(String title);

  private static native int[] nativeEvents();
}
