package android.view;
public class KeyEvent extends InputEvent {
  public int getSource() {
    return source;
  }
  public int getDeviceId() {
    return device;
  }
  public long getEventTime() {
    return eventTime;
  }
  public static final int ACTION_DOWN = 0, ACTION_UP = 1, KEYCODE_BACK = 4;
  public static final int FLAG_SOFT_KEYBOARD = 2, FLAG_KEEP_TOUCH_MODE = 4, FLAG_EDITOR_ACTION = 16;
  public static final int KEYCODE_UNKNOWN = 0, KEYCODE_0 = 7, KEYCODE_STAR = 17, KEYCODE_POUND = 18,
      KEYCODE_A = 29, KEYCODE_COMMA = 55, KEYCODE_PERIOD = 56, KEYCODE_ALT_LEFT = 57, KEYCODE_ALT_RIGHT = 58,
      KEYCODE_SHIFT_LEFT = 59, KEYCODE_SHIFT_RIGHT = 60, KEYCODE_TAB = 61, KEYCODE_SPACE = 62, KEYCODE_SYM = 63,
      KEYCODE_ENTER = 66, KEYCODE_DEL = 67, KEYCODE_GRAVE = 68, KEYCODE_MINUS = 69, KEYCODE_EQUALS = 70,
      KEYCODE_LEFT_BRACKET = 71, KEYCODE_RIGHT_BRACKET = 72, KEYCODE_BACKSLASH = 73, KEYCODE_SEMICOLON = 74,
      KEYCODE_APOSTROPHE = 75, KEYCODE_SLASH = 76, KEYCODE_AT = 77, KEYCODE_PLUS = 81, KEYCODE_ESCAPE = 111,
      KEYCODE_CTRL_LEFT = 113, KEYCODE_CTRL_RIGHT = 114, KEYCODE_CAPS_LOCK = 115, KEYCODE_SCROLL_LOCK = 116,
      KEYCODE_META_LEFT = 117, KEYCODE_META_RIGHT = 118, KEYCODE_FUNCTION = 119, KEYCODE_NUM_LOCK = 143,
      KEYCODE_NUMPAD_0 = 144, KEYCODE_NUMPAD_DIVIDE = 154, KEYCODE_NUMPAD_MULTIPLY = 155,
      KEYCODE_NUMPAD_SUBTRACT = 156, KEYCODE_NUMPAD_ADD = 157, KEYCODE_NUMPAD_DOT = 158,
      KEYCODE_NUMPAD_COMMA = 159, KEYCODE_NUMPAD_ENTER = 160, KEYCODE_NUMPAD_EQUALS = 161,
      KEYCODE_NUMPAD_LEFT_PAREN = 162, KEYCODE_NUMPAD_RIGHT_PAREN = 163;
  public static final int META_SHIFT_ON = 0x1, META_ALT_ON = 0x2, META_SYM_ON = 0x4, META_FUNCTION_ON = 0x8,
      META_ALT_LEFT_ON = 0x10, META_ALT_RIGHT_ON = 0x20, META_SHIFT_LEFT_ON = 0x40, META_SHIFT_RIGHT_ON = 0x80,
      META_CTRL_ON = 0x1000, META_CTRL_LEFT_ON = 0x2000, META_CTRL_RIGHT_ON = 0x4000, META_META_ON = 0x10000,
      META_META_LEFT_ON = 0x20000, META_META_RIGHT_ON = 0x40000, META_CAPS_LOCK_ON = 0x100000,
      META_NUM_LOCK_ON = 0x200000, META_SCROLL_LOCK_ON = 0x400000;
  public static final int META_SHIFT_MASK = META_SHIFT_ON | META_SHIFT_LEFT_ON | META_SHIFT_RIGHT_ON,
      META_ALT_MASK = META_ALT_ON | META_ALT_LEFT_ON | META_ALT_RIGHT_ON,
      META_CTRL_MASK = META_CTRL_ON | META_CTRL_LEFT_ON | META_CTRL_RIGHT_ON,
      META_META_MASK = META_META_ON | META_META_LEFT_ON | META_META_RIGHT_ON;
  private static final int META_CAP_LOCKED = 0x100, META_ALT_LOCKED = 0x200, META_SYM_LOCKED = 0x400;
  private static final int META_MODIFIER_MASK = META_SHIFT_MASK | META_ALT_MASK | META_CTRL_MASK | META_META_MASK
      | META_SYM_ON | META_FUNCTION_ON;
  private static final int META_LOCK_MASK = META_CAPS_LOCK_ON | META_NUM_LOCK_ON | META_SCROLL_LOCK_ON;
  /** API 32's last key code (KEYCODE_PROFILE_SWITCH). */
  public static int getMaxKeyCode() { return 288; }
  private final int action, code;
  private final long downTime, eventTime;
  private final int repeat, meta, device, scan, flags, source;
  public KeyEvent(int a, int c) {
    this(android.os.SystemClock.uptimeMillis(), android.os.SystemClock.uptimeMillis(), a, c, 0);
  }
  public KeyEvent(long down, long event, int action, int code, int repeat) {
    this(down, event, action, code, repeat, 0, -1, 0, 0, 0x101);
  }
  public KeyEvent(long down, long event, int action, int code, int repeat, int meta) {
    this(down, event, action, code, repeat, meta, -1, 0, 0, 0x101);
  }
  public KeyEvent(long down, long event, int action, int code, int repeat, int meta,
      int device, int scan, int flags, int source) {
    this.downTime = down; this.eventTime = event; this.action = action; this.code = code;
    this.repeat = repeat; this.meta = meta; this.device = device; this.scan = scan;
    this.flags = flags; this.source = source;
  }
  public long getDownTime() { return downTime; }
  public int getMetaState() { return meta; }
  public int getModifiers() { return normalizeMetaState(meta) & META_MODIFIER_MASK; }
  public int getScanCode() { return scan; }
  public int getFlags() { return flags; }
  // AOSP: side bits imply their "either" bit, legacy locked bits their current ones.
  public static int normalizeMetaState(int state) {
    if ((state & (META_SHIFT_LEFT_ON | META_SHIFT_RIGHT_ON)) != 0) state |= META_SHIFT_ON;
    if ((state & (META_ALT_LEFT_ON | META_ALT_RIGHT_ON)) != 0) state |= META_ALT_ON;
    if ((state & (META_CTRL_LEFT_ON | META_CTRL_RIGHT_ON)) != 0) state |= META_CTRL_ON;
    if ((state & (META_META_LEFT_ON | META_META_RIGHT_ON)) != 0) state |= META_META_ON;
    if ((state & META_CAP_LOCKED) != 0) state |= META_CAPS_LOCK_ON;
    if ((state & META_ALT_LOCKED) != 0) state |= META_ALT_ON;
    if ((state & META_SYM_LOCKED) != 0) state |= META_SYM_ON;
    return state & (META_MODIFIER_MASK | META_LOCK_MASK);
  }
  public final KeyCharacterMap getKeyCharacterMap() { return KeyCharacterMap.load(device); }
  public char getDisplayLabel() { return getKeyCharacterMap().getDisplayLabel(code); }
  public int getUnicodeChar() { return getUnicodeChar(meta); }
  public int getUnicodeChar(int state) { return getKeyCharacterMap().get(code, state); }
  public char getNumber() { return getKeyCharacterMap().getNumber(code); }
  public char getMatch(char[] chars) { return getMatch(chars, 0); }
  public char getMatch(char[] chars, int metaState) { return getKeyCharacterMap().getMatch(code, chars, metaState); }
  public boolean isPrintingKey() { return getKeyCharacterMap().isPrintingKey(code); }
  @Deprecated
  public boolean getKeyData(KeyCharacterMap.KeyData results) { return getKeyCharacterMap().getKeyData(code, results); }
  public static boolean isModifierKey(int keyCode) {
    return switch (keyCode) {
      case KEYCODE_SHIFT_LEFT, KEYCODE_SHIFT_RIGHT, KEYCODE_ALT_LEFT, KEYCODE_ALT_RIGHT, KEYCODE_CTRL_LEFT,
          KEYCODE_CTRL_RIGHT, KEYCODE_META_LEFT, KEYCODE_META_RIGHT, KEYCODE_SYM, 78 /* NUM */, KEYCODE_FUNCTION -> true;
      default -> false;
    };
  }
  public int getAction() {
    return action;
  }
  public int getKeyCode() {
    return code;
  }
  public int getRepeatCount() {
    return repeat;
  }
  public interface Callback {
    boolean onKeyDown(int code, KeyEvent event);
    boolean onKeyLongPress(int code, KeyEvent event);
    boolean onKeyUp(int code, KeyEvent event);
    boolean onKeyMultiple(int code, int count, KeyEvent event);
  }
  public boolean hasNoModifiers() { return getModifiers() == 0; }
  public boolean hasModifiers(int modifiers) { return getModifiers() == (normalizeMetaState(modifiers) & 0x770ff); }
  // AOSP KeyEvent.isSystemKey: keys the platform handles before applications.
  public final boolean isSystem() { return isSystemKey(code); }
  public static boolean isSystemKey(int code) {
    return switch (code) {
      case 1, 2, 3, 4, 5, 6, 24, 25, 26, 27, 79, 80, 82, 84, 85, 86, 87, 88, 89, 90, 91, 126, 127, 130, 164,
          220, 221, 222, 280, 281, 282, 283 -> true;
      default -> false;
    };
  }
}
