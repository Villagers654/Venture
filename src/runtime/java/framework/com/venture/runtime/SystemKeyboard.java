package com.venture.runtime;

import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.text.InputType;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.View;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.ExtractedTextRequest;
import android.view.inputmethod.InputConnection;
import java.awt.BasicStroke;
import java.awt.Color;
import java.awt.Font;
import java.awt.Graphics2D;
import java.awt.RenderingHints;
import java.awt.Shape;
import java.awt.geom.Path2D;
import java.awt.geom.RoundRectangle2D;
import java.awt.image.BufferedImage;
import java.awt.image.DataBufferInt;
import java.util.ArrayList;
import java.util.Locale;

/**
 * The keyboard Horizon OS shows for a text field (InputMethodManager.showSoftInput).
 * Venture draws it here; the host shows the picture as a panel in front of the
 * player (xr/system_keyboard.h), pointed at with either controller and clicked
 * with the trigger, and the desktop mirror window types from the PC keyboard at
 * the same time. Every edit goes through the field's own InputConnection on the
 * main Looper, so Unity's EditText, GameActivity's GameTextInput and any other
 * editor see what an Android input method would send.
 *
 * Drawn only when the picture changes (an edit, a page, the key a controller
 * points at); the host keeps the newest picture and the key rectangles, works
 * out which key each controller points at and queues events, which this class
 * reads on the main Looper while it is shown.
 */
public final class SystemKeyboard {
  public static final int WIDTH = 1024, HEIGHT = 448;
  // Host events (xr/system_keyboard.h): kind, two values and the layout they hit.
  public static final int HOVER = 0, PRESS = 1, RELEASE = 2, TEXT = 3, KEY = 4;
  // Keys that type nothing; a typing key's code is its code point.
  static final int SHIFT = -1, SYMBOLS = -2, MORE = -3, ACTION = -4, DELETE = -5, CLOSE = -6, LETTERS = -7;
  private record Key(String name, int code, int x, int y, int w, int h) {}

  private static final int MARGIN = 16, GAP = 8, UNIT = 92, BAR = 64, TOP = 96, ROW = 78, POLL_MS = 8;
  private static final String[][] ROWS = {
      {"qwertyuiop", "asdfghjkl", "zxcvbnm"},
      {"1234567890", "@#$%&-+()", "*\"':;!?"},
      {"~`|^_={}[]", "<>/\\€£¥¢°", "©®™§¶•…"}};
  private static final Color PANEL = new Color(0x1b1d22), FIELD = new Color(0x0e1013), KEY_FILL = new Color(0x30343c),
      SPECIAL = new Color(0x24272e), ACCENT = new Color(0x1a73e8), ACTIVE = new Color(0x4a5263),
      TEXT_COLOR = new Color(0xf1f3f4), HINT = new Color(0x8a9099), CARET = new Color(0x8ab4f8),
      RING = new Color(0xc8d0dc);

  private static View view, last;
  private static InputConnection connection;
  private static EditorInfo editor;
  private static int page, layout, served;
  private static boolean shift, caps;
  private static long lastShift;
  private static Key[] keys = {};
  private static final int[] hover = {-1, -1}, pressed = {-1, -1};
  private static boolean host = true, polling;
  private static Handler main;
  private static final Runnable POLL = SystemKeyboard::poll, REPEAT = SystemKeyboard::repeatDelete;

  private SystemKeyboard() {}

  private static native void nativeShow(int[] premultipliedArgb, int width, int height, int[] keys, int layout);
  private static native void nativeHide();
  private static native int[] nativeEvents();

  // ---- InputMethodManager ---------------------------------------------------------
  /** Opens the keyboard on the view's connection; false when the view edits no text. */
  public static synchronized boolean show(View target) {
    if (target == null) return false;
    var info = new EditorInfo();
    InputConnection ic;
    try {
      ic = target.onCreateInputConnection(info);
    } catch (RuntimeException failure) {
      log(android.util.Log.WARN, "The text field refused an input connection: " + failure);
      return false;
    }
    if (ic == null) return false;
    boolean moved = target != view;
    if (moved && connection != null) finish(connection);
    view = last = target;
    connection = ic;
    editor = info;
    if (moved) {
      ++served;
      int type = info.inputType & InputType.TYPE_MASK_CLASS;
      page = type == InputType.TYPE_CLASS_NUMBER || type == InputType.TYPE_CLASS_PHONE
          || type == InputType.TYPE_CLASS_DATETIME ? 1 : 0;
      shift = caps = false;
      layoutKeys();
      log(android.util.Log.INFO, String.format("Shown for %s: inputType=0x%x imeOptions=0x%x",
          target.getClass().getName(), info.inputType, info.imeOptions));
    }
    autoShift();
    publish();
    poll(false);
    return true;
  }
  public static synchronized boolean hide() {
    if (view == null) return false;
    finish(connection);
    view = null;
    connection = null;
    editor = null;
    ++served;
    pressed[0] = pressed[1] = hover[0] = hover[1] = -1;
    if (main != null) main.removeCallbacks(REPEAT);
    if (host) {
      try {
        nativeHide();
      } catch (UnsatisfiedLinkError unbound) {
        host = false;
      }
    }
    return true;
  }
  /** InputMethodManager.restartInput: the served view's new connection and editor details. */
  public static synchronized void restart(View target) {
    if (target == null || target != view) return;
    var info = new EditorInfo();
    var ic = target.onCreateInputConnection(info);
    if (ic == null) {
      hide();
      return;
    }
    if (ic != connection) finish(connection);
    connection = ic;
    editor = info;
    autoShift();
    publish();
  }
  /** The field's text changed outside the keyboard: the preview follows it. */
  public static synchronized void textChanged(View target) {
    if (target != null && target == view) publish();
  }
  public static synchronized void toggle() {
    if (view != null) hide();
    else if (last != null) show(last);
  }
  public static synchronized boolean isShown() { return view != null; }
  public static synchronized View servedView() { return view; }

  // ---- Host events ----------------------------------------------------------------
  /** One host event (also the tests' entry point); runs on the main Looper. */
  public static synchronized void onHostEvent(int kind, int a, int b, int at) {
    if (view == null) return;
    switch (kind) {
      case HOVER -> {
        if (at != layout || a < 0 || a > 1) return;
        int key = b >= 0 && b < keys.length ? b : -1;
        if (hover[a] == key) return;
        hover[a] = key;
      }
      case PRESS -> {
        if (at != layout || a < 0 || a > 1 || b < 0 || b >= keys.length) return;
        pressed[a] = b;
        activate(keys[b]);
      }
      case RELEASE -> {
        if (a < 0 || a > 1 || pressed[a] < 0) return;
        pressed[a] = -1;
        if (!deleteHeld() && main != null) main.removeCallbacks(REPEAT);
      }
      case TEXT -> {
        if (!Character.isValidCodePoint(a) || Character.isISOControl(a)) return;
        commit(new String(Character.toChars(a)));
      }
      case KEY -> {
        switch (a) {
          case KeyEvent.KEYCODE_DEL -> delete();
          case KeyEvent.KEYCODE_ENTER -> action();
          case KeyEvent.KEYCODE_ESCAPE, KeyEvent.KEYCODE_BACK -> close();
          default -> { return; }
        }
      }
      default -> { return; }
    }
    publish();
  }
  /** The index of the key with this name on the current page ("q", "shift", "delete", "action", ...). */
  public static synchronized int keyIndex(String name) {
    for (int i = 0; i < keys.length; ++i)
      if (keys[i].name.equals(name)) return i;
    return -1;
  }
  public static synchronized int layout() { return layout; }
  /** What the keyboard's text line shows: the field's text, masked for a password. */
  public static synchronized String preview() {
    if (view == null) return "";
    var text = fieldText();
    return shown(text[0] + text[1]);
  }

  private static void log(int priority, String message) {
    try {
      android.util.Log.println(priority, "SystemKeyboard", message);
    } catch (UnsatisfiedLinkError unconfigured) {
      // A JVM without the runtime (the framework tests) has no log until configured.
    }
  }
  private static void poll(boolean due) {
    synchronized (SystemKeyboard.class) {
      if (due) polling = false;
      if (view == null || !host) return;
      if (due) {
        int[] events;
        try {
          events = nativeEvents();
        } catch (UnsatisfiedLinkError unbound) {
          host = false;  // a JVM without the runtime (the framework tests)
          return;
        }
        for (int i = 0; events != null && i + 3 < events.length && view != null; i += 4)
          onHostEvent(events[i], events[i + 1], events[i + 2], events[i + 3]);
      }
      if (polling || view == null) return;
      if (main == null) {
        var looper = Looper.getMainLooper();
        if (looper == null) return;
        main = new Handler(looper);
      }
      polling = main.postDelayed(POLL, POLL_MS);
    }
  }
  private static void poll() { poll(true); }

  // ---- Editing ----------------------------------------------------------------------
  private static void activate(Key key) {
    switch (key.code) {
      case SHIFT -> {
        long now = SystemClock.uptimeMillis();
        if (caps) shift = caps = false;
        else if (shift && now - lastShift < 400) caps = true;
        else shift = !shift;
        lastShift = now;
      }
      case SYMBOLS -> setPage(1);
      case MORE -> setPage(2);
      case LETTERS -> setPage(0);
      case DELETE -> {
        delete();
        if (main != null) {
          main.removeCallbacks(REPEAT);
          main.postDelayed(REPEAT, 450);
        }
      }
      case ACTION -> action();
      case CLOSE -> close();
      default -> {
        var text = new String(Character.toChars(key.code));
        if (page == 0 && (shift || caps)) text = text.toUpperCase(Locale.ROOT);
        if (!caps) shift = false;
        commit(text);
      }
    }
  }
  private static void setPage(int next) {
    page = next;
    layoutKeys();
  }
  private static void commit(String text) {
    if (connection == null) return;
    connection.commitText(text, 1);
    autoShift();
  }
  // LatinIME: a selection is deleted whole, otherwise one code point; a field
  // that reports no text before the cursor gets the Delete key itself.
  private static void delete() {
    if (connection == null) return;
    var selected = connection.getSelectedText(0);
    if (selected != null && selected.length() > 0) {
      connection.commitText("", 1);
    } else {
      var before = connection.getTextBeforeCursor(2, 0);
      if (before == null || before.length() == 0) sendKey(KeyEvent.KEYCODE_DEL);
      else connection.deleteSurroundingText(Character.charCount(Character.codePointBefore(before, before.length())), 0);
    }
    autoShift();
  }
  private static boolean deleteHeld() {
    for (int key : pressed)
      if (key >= 0 && key < keys.length && keys[key].code == DELETE) return true;
    return false;
  }
  private static void repeatDelete() {
    synchronized (SystemKeyboard.class) {
      if (view == null || !deleteHeld()) return;
      delete();
      publish();
      main.postDelayed(REPEAT, 60);
    }
  }
  // Multi-line fields take Enter as a newline; others get their editor action
  // (LatinIME: Enter for IME_ACTION_NONE). Done, Go, Search and Send close the
  // keyboard as the headset's does, unless the game moved to another field.
  private static void action() {
    if (connection == null) return;
    if (newline()) {
      commit("\n");
      return;
    }
    int id = editor.actionLabel != null ? editor.actionId : editor.imeOptions & EditorInfo.IME_MASK_ACTION;
    if (id == EditorInfo.IME_ACTION_NONE) {
      sendKey(KeyEvent.KEYCODE_ENTER);
      return;
    }
    int before = served;
    connection.performEditorAction(id);
    if (served == before && view != null && id != EditorInfo.IME_ACTION_NEXT && id != EditorInfo.IME_ACTION_PREVIOUS)
      hide();
  }
  // The close key is Back with the keyboard up: the field sees it first
  // (AOSP onKeyPreIme; Unity's field finishes its edit there), then it closes.
  private static void close() {
    var target = view;
    int before = served;
    long now = SystemClock.uptimeMillis();
    for (int action : new int[] {KeyEvent.ACTION_DOWN, KeyEvent.ACTION_UP})
      target.dispatchKeyEventPreIme(new KeyEvent(now, now, action, KeyEvent.KEYCODE_BACK, 0, 0,
          KeyCharacterMap.VIRTUAL_KEYBOARD, 0, KeyEvent.FLAG_SOFT_KEYBOARD | KeyEvent.FLAG_KEEP_TOUCH_MODE, 0x101));
    if (served == before) hide();
  }
  private static void sendKey(int code) {
    long now = SystemClock.uptimeMillis();
    for (int action : new int[] {KeyEvent.ACTION_DOWN, KeyEvent.ACTION_UP})
      connection.sendKeyEvent(new KeyEvent(now, now, action, code, 0, 0, KeyCharacterMap.VIRTUAL_KEYBOARD, 0,
          KeyEvent.FLAG_SOFT_KEYBOARD | KeyEvent.FLAG_KEEP_TOUCH_MODE, 0x101));
  }
  private static void finish(InputConnection ic) {
    try {
      ic.finishComposingText();
    } catch (RuntimeException ignored) {
      // The field's own connection: its failure must not keep the keyboard up.
    }
  }
  private static boolean newline() {
    int type = editor.inputType;
    return (editor.imeOptions & EditorInfo.IME_FLAG_NO_ENTER_ACTION) != 0
        || ((type & InputType.TYPE_MASK_CLASS) == InputType.TYPE_CLASS_TEXT
            && (type & (InputType.TYPE_TEXT_FLAG_MULTI_LINE | InputType.TYPE_TEXT_FLAG_IME_MULTI_LINE)) != 0);
  }
  private static void autoShift() {
    if (caps || connection == null || page != 0) return;
    int type = editor.inputType;
    if ((type & InputType.TYPE_MASK_CLASS) != InputType.TYPE_CLASS_TEXT) return;
    if ((type & InputType.TYPE_TEXT_FLAG_CAP_CHARACTERS) != 0) shift = true;
    else if ((type & (InputType.TYPE_TEXT_FLAG_CAP_WORDS | InputType.TYPE_TEXT_FLAG_CAP_SENTENCES)) != 0)
      shift = connection.getCursorCapsMode(type) != 0;
  }
  private static boolean masked() {
    int type = editor.inputType, variation = type & InputType.TYPE_MASK_VARIATION;
    return switch (type & InputType.TYPE_MASK_CLASS) {
      case InputType.TYPE_CLASS_TEXT -> variation == InputType.TYPE_TEXT_VARIATION_PASSWORD
          || variation == InputType.TYPE_TEXT_VARIATION_WEB_PASSWORD;
      case InputType.TYPE_CLASS_NUMBER -> variation == InputType.TYPE_NUMBER_VARIATION_PASSWORD;
      default -> false;
    };
  }
  private static String shown(String text) {
    text = text.replace('\n', ' ');
    return masked() ? "•".repeat(text.codePointCount(0, text.length())) : text;
  }
  // The text before and after the cursor, from the extracted text where the
  // connection gives it (whole), else around the cursor.
  private static String[] fieldText() {
    try {
      var extracted = connection.getExtractedText(new ExtractedTextRequest(), 0);
      if (extracted != null && extracted.text != null) {
        var text = extracted.text.toString();
        int cursor = Math.max(0, Math.min(text.length(), extracted.selectionEnd));
        return new String[] {text.substring(0, cursor), text.substring(cursor)};
      }
      var before = connection.getTextBeforeCursor(512, 0);
      var after = connection.getTextAfterCursor(512, 0);
      return new String[] {before == null ? "" : before.toString(), after == null ? "" : after.toString()};
    } catch (RuntimeException failure) {
      return new String[] {"", ""};
    }
  }
  private static String actionLabel() {
    if (newline()) return "Enter";
    if (editor.actionLabel != null) return editor.actionLabel.toString();
    return switch (editor.imeOptions & EditorInfo.IME_MASK_ACTION) {
      case EditorInfo.IME_ACTION_GO -> "Go";
      case EditorInfo.IME_ACTION_SEARCH -> "Search";
      case EditorInfo.IME_ACTION_SEND -> "Send";
      case EditorInfo.IME_ACTION_NEXT -> "Next";
      case EditorInfo.IME_ACTION_PREVIOUS -> "Prev";
      case EditorInfo.IME_ACTION_NONE -> "Enter";
      default -> "Done";
    };
  }

  // ---- Layout and drawing ---------------------------------------------------------
  private static int span(double units) { return (int) Math.round(units * (UNIT + GAP) - GAP); }
  private record Spec(String name, int code, double units) {
    static Spec of(int c) { return new Spec(new String(Character.toChars(c)), c, 1); }
  }
  private static void layoutKeys() {
    var list = new ArrayList<Key>();
    list.add(new Key("close", CLOSE, WIDTH - MARGIN - UNIT, MARGIN, UNIT, BAR));
    var rows = ROWS[page];
    for (int r = 0; r < 4; ++r) {
      var row = new ArrayList<Spec>();
      if (r == 2) row.add(page == 0 ? new Spec("shift", SHIFT, 1.5)
          : page == 1 ? new Spec("=\\<", MORE, 1.5) : new Spec("?123", SYMBOLS, 1.5));
      if (r < 3) rows[r].codePoints().forEach(c -> row.add(Spec.of(c)));
      if (r == 2) row.add(new Spec("delete", DELETE, 1.5));
      if (r == 3) {
        row.add(page == 0 ? new Spec("?123", SYMBOLS, 1.5) : new Spec("ABC", LETTERS, 1.5));
        row.add(Spec.of(','));
        row.add(new Spec("space", ' ', 5));
        row.add(Spec.of('.'));
        row.add(new Spec("action", ACTION, 1.5));
      }
      int x = MARGIN + (r == 1 ? span(0.5) + GAP : 0), y = TOP + r * (ROW + GAP);
      for (var spec : row) {
        int w = span(spec.units);
        list.add(new Key(spec.name, spec.code, x, y, w, ROW));
        x += w + GAP;
      }
    }
    keys = list.toArray(new Key[0]);
    ++layout;
    pressed[0] = pressed[1] = hover[0] = hover[1] = -1;
    if (main != null) main.removeCallbacks(REPEAT);
  }
  private static void publish() {
    if (view == null) return;
    int[] pixels = render();
    if (!host) return;
    int[] rects = new int[keys.length * 4];
    for (int i = 0; i < keys.length; ++i) {
      rects[i * 4] = keys[i].x;
      rects[i * 4 + 1] = keys[i].y;
      rects[i * 4 + 2] = keys[i].w;
      rects[i * 4 + 3] = keys[i].h;
    }
    try {
      nativeShow(pixels, WIDTH, HEIGHT, rects, layout);
    } catch (UnsatisfiedLinkError unbound) {
      host = false;
    }
  }
  private static Boolean fonts;
  private static boolean fonts() {
    if (fonts == null) {
      try {
        new Font(Font.SANS_SERIF, Font.PLAIN, 12).getLineMetrics("Ag", new java.awt.font.FontRenderContext(null, true, true));
        fonts = true;
      } catch (Throwable unavailable) {  // Java2D without the host's font configuration
        fonts = false;
      }
    }
    return fonts;
  }
  // Premultiplied ARGB, row 0 at the top.
  private static int[] render() {
    var image = new BufferedImage(WIDTH, HEIGHT, BufferedImage.TYPE_INT_ARGB_PRE);
    var g = image.createGraphics();
    try {
      g.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_ON);
      g.setRenderingHint(RenderingHints.KEY_TEXT_ANTIALIASING, RenderingHints.VALUE_TEXT_ANTIALIAS_ON);
      g.setRenderingHint(RenderingHints.KEY_STROKE_CONTROL, RenderingHints.VALUE_STROKE_PURE);
      g.setColor(PANEL);
      g.fill(new RoundRectangle2D.Float(0, 0, WIDTH, HEIGHT, 44, 44));
      drawField(g);
      for (int i = 0; i < keys.length; ++i) drawKey(g, i);
    } finally {
      g.dispose();
    }
    return ((DataBufferInt) image.getRaster().getDataBuffer()).getData();
  }
  private static void drawField(Graphics2D g) {
    int x = MARGIN, y = MARGIN, w = WIDTH - 2 * MARGIN - UNIT - GAP, h = BAR, pad = 20;
    g.setColor(FIELD);
    g.fill(new RoundRectangle2D.Float(x, y, w, h, 20, 20));
    var text = fieldText();
    String before = shown(text[0]), after = shown(text[1]);
    var clip = g.getClip();
    g.clipRect(x + pad - 4, y, w - 2 * pad + 8, h);
    int caretX = x + pad;
    if (fonts()) {
      try {
        g.setFont(new Font(Font.SANS_SERIF, Font.PLAIN, 30));
        var metrics = g.getFontMetrics();
        int baseline = y + (h - metrics.getHeight()) / 2 + metrics.getAscent();
        if (before.isEmpty() && after.isEmpty()) {
          var hint = editor.hintText != null ? editor.hintText : editor.label;
          if (hint != null) {
            g.setColor(HINT);
            g.drawString(hint.toString(), x + pad, baseline);
          }
        } else {
          // Keep the cursor in view: a long text scrolls left.
          int width = metrics.stringWidth(before), room = w - 2 * pad;
          int left = x + pad - Math.max(0, width - room);
          g.setColor(TEXT_COLOR);
          g.drawString(before + after, left, baseline);
          caretX = left + width;
        }
      } catch (Throwable unavailable) {
        fonts = false;
      }
    }
    g.setColor(CARET);
    g.fillRect(caretX, y + 14, 3, h - 28);
    g.setClip(clip);
  }
  private static void drawKey(Graphics2D g, int index) {
    var key = keys[index];
    boolean hot = hover[0] == index || hover[1] == index, down = pressed[0] == index || pressed[1] == index;
    Color fill = key.code == ACTION ? ACCENT : key.code == SHIFT && (shift || caps) ? ACTIVE
        : key.code < 0 ? SPECIAL : KEY_FILL;
    if (down) fill = key.code == ACTION ? fill.darker() : ACCENT;
    else if (hot) fill = lighter(fill);
    var shape = new RoundRectangle2D.Float(key.x, key.y, key.w, key.h, 20, 20);
    g.setColor(fill);
    g.fill(shape);
    if (hot && !down) {
      g.setColor(RING);
      g.setStroke(new BasicStroke(2.5f));
      g.draw(new RoundRectangle2D.Float(key.x + 1.25f, key.y + 1.25f, key.w - 2.5f, key.h - 2.5f, 18, 18));
    }
    float cx = key.x + key.w / 2f, cy = key.y + key.h / 2f;
    g.setColor(TEXT_COLOR);
    switch (key.code) {
      case SHIFT -> {
        var arrow = new Path2D.Float();
        arrow.moveTo(cx, cy - 18); arrow.lineTo(cx + 17, cy); arrow.lineTo(cx + 8, cy); arrow.lineTo(cx + 8, cy + 14);
        arrow.lineTo(cx - 8, cy + 14); arrow.lineTo(cx - 8, cy); arrow.lineTo(cx - 17, cy); arrow.closePath();
        stroke(g, arrow, shift || caps);
        if (caps) g.fillRect((int) cx - 8, (int) cy + 19, 16, 3);
      }
      case DELETE -> {
        var body = new Path2D.Float();
        body.moveTo(cx - 24, cy); body.lineTo(cx - 12, cy - 14); body.lineTo(cx + 22, cy - 14);
        body.lineTo(cx + 22, cy + 14); body.lineTo(cx - 12, cy + 14); body.closePath();
        stroke(g, body, false);
        var cross = new Path2D.Float();
        cross.moveTo(cx - 3, cy - 6); cross.lineTo(cx + 9, cy + 6); cross.moveTo(cx + 9, cy - 6); cross.lineTo(cx - 3, cy + 6);
        stroke(g, cross, false);
      }
      case CLOSE -> {
        var chevron = new Path2D.Float();
        chevron.moveTo(cx - 14, cy - 6); chevron.lineTo(cx, cy + 8); chevron.lineTo(cx + 14, cy - 6);
        stroke(g, chevron, false);
      }
      case ' ' -> {}
      default -> {
        String label = switch (key.code) {
          case ACTION -> actionLabel();
          case SYMBOLS, MORE, LETTERS -> key.name;
          default -> page == 0 && (shift || caps) ? key.name.toUpperCase(Locale.ROOT) : key.name;
        };
        label(g, label, cx, cy, key.code < 0 ? 26 : 32);
      }
    }
  }
  private static void stroke(Graphics2D g, Shape shape, boolean filled) {
    g.setStroke(new BasicStroke(2.6f, BasicStroke.CAP_ROUND, BasicStroke.JOIN_ROUND));
    if (filled) g.fill(shape);
    g.draw(shape);
  }
  private static void label(Graphics2D g, String text, float cx, float cy, int size) {
    if (!fonts()) return;
    try {
      g.setFont(new Font(Font.SANS_SERIF, Font.PLAIN, size));
      var metrics = g.getFontMetrics();
      g.drawString(text, cx - metrics.stringWidth(text) / 2f, cy - metrics.getHeight() / 2f + metrics.getAscent());
    } catch (Throwable unavailable) {
      fonts = false;
    }
  }
  private static Color lighter(Color c) {
    return new Color(Math.min(255, c.getRed() + 26), Math.min(255, c.getGreen() + 26), Math.min(255, c.getBlue() + 30));
  }
}
