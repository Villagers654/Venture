package com.venture.runtime.layoutlib;

import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.VentureWindow;
import android.view.View;
import android.view.ViewGroup;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;

/**
 * Scripted input for a 2D app, for development runs: VENTURE_PANEL_SCRIPT names
 * a file of steps, one per line, sent through the panel's input path as the
 * host window's input is. Each step waits for the views it needs, so a run
 * takes as long as the app does:
 *
 *   tap <text>        a shown view whose text or content description is <text>
 *   tap <x> <y>       panel pixels
 *   wait <text>       until a shown view has <text>
 *   sleep <ms>
 *   key <NAME>        a KeyEvent keycode name without KEYCODE_ (BACK, ENTER, TAB)
 *   type <text>       characters to the focused view, as typed text (Panel.text)
 *   pick <path>       answers the app's next document picker with a host path
 *                     instead of the host's dialog ("pick" alone cancels it)
 *   quit              the host's close request
 *
 * Blank lines and lines starting with # are ignored. A step that waits gives up
 * after VENTURE_PANEL_SCRIPT_TIMEOUT_MS (default 30000) and ends the script.
 */
final class PanelScript {
  private static final String TAG = "PanelScript";
  private final Panel panel;
  private final List<String> steps;
  private final Handler main = new Handler(Looper.getMainLooper());
  private final long timeout;
  private int next;
  private long stepStarted;

  private PanelScript(Panel panel, List<String> steps, long timeout) {
    this.panel = panel;
    this.steps = steps;
    this.timeout = timeout;
  }

  static void start(Panel panel) {
    String file = System.getenv("VENTURE_PANEL_SCRIPT");
    if (file == null || file.isEmpty()) return;
    List<String> steps = new ArrayList<>();
    try {
      for (String line : Files.readAllLines(Path.of(file), StandardCharsets.UTF_8)) {
        line = line.strip();
        if (!line.isEmpty() && !line.startsWith("#")) steps.add(line);
      }
    } catch (IOException e) {
      android.util.Log.w(TAG, "Cannot read " + file + ": " + e);
      return;
    }
    long timeout = 30000;
    String value = System.getenv("VENTURE_PANEL_SCRIPT_TIMEOUT_MS");
    if (value != null) {
      try { timeout = Long.parseLong(value); } catch (NumberFormatException ignored) { /* default */ }
    }
    PanelScript script = new PanelScript(panel, steps, timeout);
    script.stepStarted = SystemClock.uptimeMillis();
    script.main.postDelayed(script::poll, 100);
  }

  private void poll() {
    if (next >= steps.size()) {
      android.util.Log.i(TAG, "Script finished");
      return;
    }
    String step = steps.get(next);
    long delay;
    try {
      delay = run(step);
    } catch (RuntimeException e) {
      android.util.Log.w(TAG, "Step " + (next + 1) + " \"" + step + "\" failed: " + e);
      return;
    }
    if (delay < 0) {
      if (SystemClock.uptimeMillis() - stepStarted > timeout) {
        android.util.Log.w(TAG, "Step " + (next + 1) + " \"" + step + "\" timed out");
        return;
      }
      main.postDelayed(this::poll, 100);
      return;
    }
    android.util.Log.i(TAG, "Step " + (next + 1) + " \"" + step + "\" done");
    ++next;
    stepStarted = SystemClock.uptimeMillis();
    main.postDelayed(this::poll, Math.max(delay, 50));
  }

  /** Runs one step: the delay before the next, or -1 to try this one again. */
  private long run(String step) {
    int space = step.indexOf(' ');
    String verb = space < 0 ? step : step.substring(0, space);
    String argument = space < 0 ? "" : step.substring(space + 1).strip();
    switch (verb) {
      case "tap" -> {
        String[] parts = argument.split(" ");
        float[] point;
        if (parts.length == 2 && parts[0].matches("-?\\d+(\\.\\d+)?") && parts[1].matches("-?\\d+(\\.\\d+)?")) {
          point = new float[] {Float.parseFloat(parts[0]), Float.parseFloat(parts[1])};
        } else {
          point = find(argument);
          if (point == null) return -1;
        }
        tap(point[0], point[1]);
        return 300;
      }
      case "wait" -> { return find(argument) == null ? -1 : 0; }
      case "sleep" -> { return Long.parseLong(argument); }
      case "key" -> {
        int code = KeyEvent.keyCodeFromString("KEYCODE_" + argument);
        if (code == KeyEvent.KEYCODE_UNKNOWN) throw new IllegalArgumentException("unknown key " + argument);
        long now = SystemClock.uptimeMillis();
        panel.key(new KeyEvent(now, now, KeyEvent.ACTION_DOWN, code, 0, 0, KeyCharacterMap.VIRTUAL_KEYBOARD, 0, 0,
            InputDevice.SOURCE_KEYBOARD));
        panel.key(new KeyEvent(now, now, KeyEvent.ACTION_UP, code, 0, 0, KeyCharacterMap.VIRTUAL_KEYBOARD, 0, 0,
            InputDevice.SOURCE_KEYBOARD));
        return 300;
      }
      case "type" -> {
        panel.text(argument, SystemClock.uptimeMillis());
        return 300;
      }
      case "pick" -> {
        synchronized (PanelScript.class) { pick = argument; }
        return 0;
      }
      case "quit" -> {
        PanelHost.close();
        return 0;
      }
      default -> throw new IllegalArgumentException("unknown step");
    }
  }

  private static String pick;

  /** The scripted answer for the next document picker (empty: cancel), or null. */
  static synchronized String takePick() {
    String answer = pick;
    pick = null;
    return answer;
  }

  private void tap(float x, float y) {
    long down = SystemClock.uptimeMillis();
    panel.pointer(MotionEvent.ACTION_DOWN, x, y, down, down, InputDevice.SOURCE_TOUCHSCREEN, 0);
    main.postDelayed(() -> {
      long up = SystemClock.uptimeMillis();
      panel.pointer(MotionEvent.ACTION_UP, x, y, down, up, InputDevice.SOURCE_TOUCHSCREEN, 0);
    }, 60);
  }

  /** The panel position of the topmost shown view with this text, or null. */
  private float[] find(String text) {
    List<VentureWindow> windows = panel.windows();
    for (int i = windows.size() - 1; i >= 0; --i) {
      VentureWindow window = windows.get(i);
      View root = window.view();
      if (root == null || window.removed()) continue;
      View match = find(root, text);
      if (match == null) continue;
      android.graphics.Rect visible = new android.graphics.Rect();
      match.getGlobalVisibleRect(visible);
      return new float[] {window.bounds().left + visible.exactCenterX(), window.bounds().top + visible.exactCenterY()};
    }
    return null;
  }

  // Shown and on screen: a pager's neighbouring pages are shown but clipped away.
  private static View find(View view, String text) {
    if (!view.isShown() || !view.getGlobalVisibleRect(new android.graphics.Rect())) return null;
    if (view instanceof ViewGroup group)
      for (int i = group.getChildCount() - 1; i >= 0; --i) {
        View match = find(group.getChildAt(i), text);
        if (match != null) return match;
      }
    if (view instanceof android.widget.TextView label && label.getText() != null
        && text.contentEquals(label.getText().toString().strip()))
      return view;
    CharSequence description = view.getContentDescription();
    return description != null && text.contentEquals(description.toString().strip()) ? view : null;
  }
}
