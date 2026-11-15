package android.view;

import android.os.Parcel;
import android.os.Parcelable;
import android.util.AndroidRuntimeException;
import android.util.SparseIntArray;
import java.text.Normalizer;

/**
 * AOSP android.view.KeyCharacterMap over the virtual keyboard's map (frameworks/base/data/keyboards/
 * Virtual.kcm, a full US QWERTY layout). Lookups follow libinput's KeyCharacterMap: a key's behaviors
 * are kept newest declaration first, and the first whose meta state matches decides the character.
 * The only Android input device is the virtual keyboard, so every device id loads its map, and no
 * device has any physical key.
 */
public class KeyCharacterMap implements Parcelable {
  @Deprecated public static final int BUILT_IN_KEYBOARD = 0;
  public static final int VIRTUAL_KEYBOARD = -1;
  public static final int NUMERIC = 1, PREDICTIVE = 2, ALPHA = 3, FULL = 4, SPECIAL_FUNCTION = 5;
  public static final char HEX_INPUT = (char) 0xEF00, PICKER_DIALOG_INPUT = (char) 0xEF01;
  public static final int MODIFIER_BEHAVIOR_CHORDED = 0, MODIFIER_BEHAVIOR_CHORDED_OR_TOGGLED = 1;
  public static final int COMBINING_ACCENT = 0x80000000, COMBINING_ACCENT_MASK = 0x7FFFFFFF;

  private static final int SHIFT = KeyEvent.META_SHIFT_ON, CAPS = KeyEvent.META_CAPS_LOCK_ON,
      NUM = KeyEvent.META_NUM_LOCK_ON;

  /** One key: its label, number and behaviors, newest declaration first as the parser links them. */
  private static final class Key {
    final char label;
    char number;
    int[] metas = new int[0];
    char[] chars = new char[0];
    Key(char label) { this.label = label; }
    Key on(int meta, char ch) {
      int n = metas.length;
      int[] m = new int[n + 1];
      char[] c = new char[n + 1];
      m[0] = meta;
      c[0] = ch;
      System.arraycopy(metas, 0, m, 1, n);
      System.arraycopy(chars, 0, c, 1, n);
      metas = m;
      chars = c;
      return this;
    }
    // KeyCharacterMap::Parser::finishKey: a missing number takes the key's digit, else its last
    // dial-pad symbol in behavior order.
    void finish() {
      char digit = 0, symbol = 0;
      for (char ch : chars) {
        if (ch >= '0' && ch <= '9') digit = ch;
        else if ("()#*-+,.':;/".indexOf(ch) >= 0 && ch != 0) symbol = ch;
      }
      number = digit != 0 ? digit : symbol;
    }
  }

  private static final Key[] VIRTUAL_KEYS = buildVirtualKeys();
  private static final KeyCharacterMap VIRTUAL = new KeyCharacterMap(VIRTUAL_KEYBOARD);

  private static Key[] buildVirtualKeys() {
    Key[] keys = new Key[KeyEvent.getMaxKeyCode() + 1];
    for (int i = 0; i < 26; ++i) {
      char upper = (char) ('A' + i), lower = (char) ('a' + i);
      keys[KeyEvent.KEYCODE_A + i] = new Key(upper).on(0, lower).on(SHIFT, upper).on(CAPS, upper)
          .on(SHIFT | CAPS, lower);
    }
    String shifted = ")!@#$%^&*(";
    for (int i = 0; i < 10; ++i) {
      char digit = (char) ('0' + i);
      keys[KeyEvent.KEYCODE_0 + i] = new Key(digit).on(0, digit).on(SHIFT, shifted.charAt(i));
    }
    keys[KeyEvent.KEYCODE_SPACE] = new Key(' ').on(0, ' ');
    keys[KeyEvent.KEYCODE_ENTER] = new Key('\n').on(0, '\n');
    keys[KeyEvent.KEYCODE_TAB] = new Key('\t').on(0, '\t');
    pair(keys, KeyEvent.KEYCODE_COMMA, ',', '<');
    pair(keys, KeyEvent.KEYCODE_PERIOD, '.', '>');
    pair(keys, KeyEvent.KEYCODE_SLASH, '/', '?');
    pair(keys, KeyEvent.KEYCODE_GRAVE, '`', '~');
    pair(keys, KeyEvent.KEYCODE_MINUS, '-', '_');
    pair(keys, KeyEvent.KEYCODE_EQUALS, '=', '+');
    pair(keys, KeyEvent.KEYCODE_LEFT_BRACKET, '[', '{');
    pair(keys, KeyEvent.KEYCODE_RIGHT_BRACKET, ']', '}');
    pair(keys, KeyEvent.KEYCODE_BACKSLASH, '\\', '|');
    pair(keys, KeyEvent.KEYCODE_SEMICOLON, ';', ':');
    pair(keys, KeyEvent.KEYCODE_APOSTROPHE, '\'', '"');
    // Numeric keypad: digits and the dot type only under num lock (otherwise they fall back to
    // navigation keys); operators always type.
    for (int i = 0; i < 10; ++i) {
      char digit = (char) ('0' + i);
      keys[KeyEvent.KEYCODE_NUMPAD_0 + i] = new Key(digit).on(NUM, digit);
    }
    keys[KeyEvent.KEYCODE_NUMPAD_DOT] = new Key('.').on(NUM, '.');
    single(keys, KeyEvent.KEYCODE_NUMPAD_LEFT_PAREN, '(');
    single(keys, KeyEvent.KEYCODE_NUMPAD_RIGHT_PAREN, ')');
    single(keys, KeyEvent.KEYCODE_NUMPAD_DIVIDE, '/');
    single(keys, KeyEvent.KEYCODE_NUMPAD_MULTIPLY, '*');
    single(keys, KeyEvent.KEYCODE_NUMPAD_SUBTRACT, '-');
    single(keys, KeyEvent.KEYCODE_NUMPAD_ADD, '+');
    single(keys, KeyEvent.KEYCODE_NUMPAD_COMMA, ',');
    single(keys, KeyEvent.KEYCODE_NUMPAD_EQUALS, '=');
    single(keys, KeyEvent.KEYCODE_NUMPAD_ENTER, '\n');
    // Special keys on phones.
    single(keys, KeyEvent.KEYCODE_AT, '@');
    single(keys, KeyEvent.KEYCODE_STAR, '*');
    single(keys, KeyEvent.KEYCODE_POUND, '#');
    single(keys, KeyEvent.KEYCODE_PLUS, '+');
    for (Key key : keys) if (key != null) key.finish();
    return keys;
  }
  private static void pair(Key[] keys, int code, char base, char shift) {
    keys[code] = new Key(base).on(0, base).on(SHIFT, shift);
  }
  private static void single(Key[] keys, int code, char ch) {
    keys[code] = new Key(ch).on(0, ch);
  }

  private final int deviceId;
  private KeyCharacterMap(int deviceId) {
    this.deviceId = deviceId;
  }

  /** The map of an input device; a device that does not exist gets the virtual keyboard's, as on Android. */
  public static KeyCharacterMap load(int deviceId) {
    return VIRTUAL;
  }

  private static Key key(int keyCode) {
    return keyCode >= 0 && keyCode < VIRTUAL_KEYS.length ? VIRTUAL_KEYS[keyCode] : null;
  }
  // KeyCharacterMap::matchesMetaState: the behavior's meta bits must all be set, and CTRL, ALT and
  // META held in the event must be exactly those the behavior names (either side of a pair counts).
  private static boolean matchesMetaState(int eventMetaState, int behaviorMetaState) {
    if ((eventMetaState & behaviorMetaState) != behaviorMetaState) return false;
    int exact = KeyEvent.META_CTRL_MASK | KeyEvent.META_ALT_MASK | KeyEvent.META_META_MASK;
    int unmatched = eventMetaState & ~behaviorMetaState & exact;
    unmatched = clearPair(unmatched, behaviorMetaState, KeyEvent.META_CTRL_ON,
        KeyEvent.META_CTRL_LEFT_ON | KeyEvent.META_CTRL_RIGHT_ON);
    unmatched = clearPair(unmatched, behaviorMetaState, KeyEvent.META_ALT_ON,
        KeyEvent.META_ALT_LEFT_ON | KeyEvent.META_ALT_RIGHT_ON);
    unmatched = clearPair(unmatched, behaviorMetaState, KeyEvent.META_META_ON,
        KeyEvent.META_META_LEFT_ON | KeyEvent.META_META_RIGHT_ON);
    return unmatched == 0;
  }
  private static int clearPair(int unmatched, int behavior, int either, int sides) {
    if ((behavior & either) != 0) return unmatched & ~sides;
    if ((behavior & sides) != 0) return unmatched & ~either;
    return unmatched;
  }
  private static char character(int keyCode, int metaState) {
    Key key = key(keyCode);
    if (key == null) return 0;
    for (int i = 0; i < key.metas.length; ++i)
      if (matchesMetaState(metaState, key.metas[i])) return key.chars[i];
    return 0;
  }

  /**
   * The Unicode character a key generates with the given meta state, 0 for none. A combining
   * diacritical mark returns its spacing accent with COMBINING_ACCENT set.
   */
  public int get(int keyCode, int metaState) {
    metaState = KeyEvent.normalizeMetaState(metaState);
    char ch = character(keyCode, metaState);
    int accent = COMBINING_TO_ACCENT.get(ch);
    return accent != 0 ? COMBINING_ACCENT | accent : ch;
  }
  public char getNumber(int keyCode) {
    Key key = key(keyCode);
    return key == null ? 0 : key.number;
  }
  public char getMatch(int keyCode, char[] chars) {
    return getMatch(keyCode, chars, 0);
  }
  /** The most general behavior's character among chars; a behavior whose meta state is held wins outright. */
  public char getMatch(int keyCode, char[] chars, int metaState) {
    if (chars == null) throw new IllegalArgumentException("chars must not be null.");
    metaState = KeyEvent.normalizeMetaState(metaState);
    Key key = key(keyCode);
    if (key == null) return 0;
    char result = 0;
    for (int i = 0; i < key.chars.length; ++i) {
      char ch = key.chars[i];
      if (ch == 0) continue;
      for (char wanted : chars) {
        if (ch == wanted) {
          result = ch;
          if ((key.metas[i] & metaState) == key.metas[i]) return result;
          break;
        }
      }
    }
    return result;
  }
  public char getDisplayLabel(int keyCode) {
    Key key = key(keyCode);
    return key == null ? 0 : key.label;
  }

  /** The character an accent followed by c composes to, 0 if none (AOSP getDeadChar). */
  public static int getDeadChar(int accent, int c) {
    if (c == accent || c == ' ') return accent;
    int combining = ACCENT_TO_COMBINING.get(accent);
    if (combining == 0) return 0;
    int combination = (combining << 16) | c;
    synchronized (DEAD_KEY_CACHE) {
      int combined = DEAD_KEY_CACHE.get(combination, -1);
      if (combined == -1) {
        String result = Normalizer.normalize(new StringBuilder().append((char) c).append((char) combining),
            Normalizer.Form.NFC);
        combined = result.codePointCount(0, result.length()) == 1 ? result.codePointAt(0) : 0;
        DEAD_KEY_CACHE.put(combination, combined);
      }
      return combined;
    }
  }

  @Deprecated
  public static class KeyData {
    public static final int META_LENGTH = 4;
    public char displayLabel;
    public char number;
    public char[] meta = new char[META_LENGTH];
  }
  @Deprecated
  public boolean getKeyData(int keyCode, KeyData results) {
    if (results.meta.length < KeyData.META_LENGTH)
      throw new IndexOutOfBoundsException("results.meta.length must be >= " + KeyData.META_LENGTH);
    char displayLabel = getDisplayLabel(keyCode);
    if (displayLabel == 0) return false;
    results.displayLabel = displayLabel;
    results.number = getNumber(keyCode);
    results.meta[0] = character(keyCode, 0);
    results.meta[1] = character(keyCode, KeyEvent.META_SHIFT_ON);
    results.meta[2] = character(keyCode, KeyEvent.META_ALT_ON);
    results.meta[3] = character(keyCode, KeyEvent.META_ALT_ON | KeyEvent.META_SHIFT_ON);
    return true;
  }

  /**
   * Key events that type chars on this map: each character presses its meta keys, the key down and
   * up, then releases the meta keys. Null when a character has no key.
   */
  public KeyEvent[] getEvents(char[] chars) {
    if (chars == null) throw new IllegalArgumentException("chars must not be null.");
    long now = android.os.SystemClock.uptimeMillis();
    java.util.ArrayList<KeyEvent> events = new java.util.ArrayList<>();
    for (char ch : chars) {
      int keyCode = -1, metaState = 0;
      // findKey: the lowest key code with a behavior for ch, using its most general behavior.
      for (int code = 0; ch != 0 && code < VIRTUAL_KEYS.length && keyCode < 0; ++code) {
        Key key = VIRTUAL_KEYS[code];
        if (key == null) continue;
        for (int i = 0; i < key.chars.length; ++i)
          if (key.chars[i] == ch) { keyCode = code; metaState = key.metas[i]; }
      }
      if (keyCode < 0) return null;
      int[] current = {0};
      addMetaKeys(events, metaState, true, now, current);
      addKey(events, keyCode, current[0], true, now);
      addKey(events, keyCode, current[0], false, now);
      addMetaKeys(events, metaState, false, now, current);
    }
    return events.toArray(new KeyEvent[0]);
  }
  private void addKey(java.util.List<KeyEvent> events, int keyCode, int metaState, boolean down, long time) {
    events.add(new KeyEvent(time, time, down ? KeyEvent.ACTION_DOWN : KeyEvent.ACTION_UP, keyCode, 0,
        metaState, deviceId, 0, 0, InputDevice.SOURCE_KEYBOARD));
  }
  private void addMetaKeys(java.util.List<KeyEvent> events, int metaState, boolean down, long time, int[] current) {
    if (down) {
      addLockedMetaKey(events, metaState, time, KeyEvent.KEYCODE_CAPS_LOCK, KeyEvent.META_CAPS_LOCK_ON, current);
      addLockedMetaKey(events, metaState, time, KeyEvent.KEYCODE_NUM_LOCK, KeyEvent.META_NUM_LOCK_ON, current);
      addLockedMetaKey(events, metaState, time, KeyEvent.KEYCODE_SCROLL_LOCK, KeyEvent.META_SCROLL_LOCK_ON, current);
      addDoubleEphemeralMetaKey(events, metaState, down, time, KeyEvent.KEYCODE_SHIFT_LEFT, KeyEvent.META_SHIFT_LEFT_ON,
          KeyEvent.KEYCODE_SHIFT_RIGHT, KeyEvent.META_SHIFT_RIGHT_ON, KeyEvent.META_SHIFT_ON, current);
      addDoubleEphemeralMetaKey(events, metaState, down, time, KeyEvent.KEYCODE_ALT_LEFT, KeyEvent.META_ALT_LEFT_ON,
          KeyEvent.KEYCODE_ALT_RIGHT, KeyEvent.META_ALT_RIGHT_ON, KeyEvent.META_ALT_ON, current);
      addDoubleEphemeralMetaKey(events, metaState, down, time, KeyEvent.KEYCODE_CTRL_LEFT, KeyEvent.META_CTRL_LEFT_ON,
          KeyEvent.KEYCODE_CTRL_RIGHT, KeyEvent.META_CTRL_RIGHT_ON, KeyEvent.META_CTRL_ON, current);
      addDoubleEphemeralMetaKey(events, metaState, down, time, KeyEvent.KEYCODE_META_LEFT, KeyEvent.META_META_LEFT_ON,
          KeyEvent.KEYCODE_META_RIGHT, KeyEvent.META_META_RIGHT_ON, KeyEvent.META_META_ON, current);
      addSingleEphemeralMetaKey(events, metaState, down, time, KeyEvent.KEYCODE_SYM, KeyEvent.META_SYM_ON, current);
      addSingleEphemeralMetaKey(events, metaState, down, time, KeyEvent.KEYCODE_FUNCTION, KeyEvent.META_FUNCTION_ON, current);
    } else {
      addSingleEphemeralMetaKey(events, metaState, down, time, KeyEvent.KEYCODE_FUNCTION, KeyEvent.META_FUNCTION_ON, current);
      addSingleEphemeralMetaKey(events, metaState, down, time, KeyEvent.KEYCODE_SYM, KeyEvent.META_SYM_ON, current);
      addDoubleEphemeralMetaKey(events, metaState, down, time, KeyEvent.KEYCODE_META_LEFT, KeyEvent.META_META_LEFT_ON,
          KeyEvent.KEYCODE_META_RIGHT, KeyEvent.META_META_RIGHT_ON, KeyEvent.META_META_ON, current);
      addDoubleEphemeralMetaKey(events, metaState, down, time, KeyEvent.KEYCODE_CTRL_LEFT, KeyEvent.META_CTRL_LEFT_ON,
          KeyEvent.KEYCODE_CTRL_RIGHT, KeyEvent.META_CTRL_RIGHT_ON, KeyEvent.META_CTRL_ON, current);
      addDoubleEphemeralMetaKey(events, metaState, down, time, KeyEvent.KEYCODE_ALT_LEFT, KeyEvent.META_ALT_LEFT_ON,
          KeyEvent.KEYCODE_ALT_RIGHT, KeyEvent.META_ALT_RIGHT_ON, KeyEvent.META_ALT_ON, current);
      addDoubleEphemeralMetaKey(events, metaState, down, time, KeyEvent.KEYCODE_SHIFT_LEFT, KeyEvent.META_SHIFT_LEFT_ON,
          KeyEvent.KEYCODE_SHIFT_RIGHT, KeyEvent.META_SHIFT_RIGHT_ON, KeyEvent.META_SHIFT_ON, current);
      addLockedMetaKey(events, metaState, time, KeyEvent.KEYCODE_SCROLL_LOCK, KeyEvent.META_SCROLL_LOCK_ON, current);
      addLockedMetaKey(events, metaState, time, KeyEvent.KEYCODE_NUM_LOCK, KeyEvent.META_NUM_LOCK_ON, current);
      addLockedMetaKey(events, metaState, time, KeyEvent.KEYCODE_CAPS_LOCK, KeyEvent.META_CAPS_LOCK_ON, current);
    }
  }
  private boolean addSingleEphemeralMetaKey(java.util.List<KeyEvent> events, int metaState, boolean down, long time,
      int keyCode, int keyMetaState, int[] current) {
    if ((metaState & keyMetaState) != keyMetaState) return false;
    current[0] = updateMetaState(keyCode, down, current[0]);
    addKey(events, keyCode, current[0], down, time);
    return true;
  }
  private void addDoubleEphemeralMetaKey(java.util.List<KeyEvent> events, int metaState, boolean down, long time,
      int leftKeyCode, int leftMeta, int rightKeyCode, int rightMeta, int eitherMeta, int[] current) {
    boolean specific = addSingleEphemeralMetaKey(events, metaState, down, time, leftKeyCode, leftMeta, current);
    specific |= addSingleEphemeralMetaKey(events, metaState, down, time, rightKeyCode, rightMeta, current);
    if (!specific) addSingleEphemeralMetaKey(events, metaState, down, time, leftKeyCode, eitherMeta, current);
  }
  private void addLockedMetaKey(java.util.List<KeyEvent> events, int metaState, long time, int keyCode,
      int keyMetaState, int[] current) {
    if ((metaState & keyMetaState) != keyMetaState) return;
    current[0] = updateMetaState(keyCode, true, current[0]);
    addKey(events, keyCode, current[0], true, time);
    current[0] = updateMetaState(keyCode, false, current[0]);
    addKey(events, keyCode, current[0], false, time);
  }
  // libinput updateMetaState: modifier keys set or clear their side bit (a release also clears the
  // "either" bits, renormalized from the other side); lock keys toggle on release.
  private static int updateMetaState(int keyCode, boolean down, int old) {
    int mask = switch (keyCode) {
      case KeyEvent.KEYCODE_ALT_LEFT -> KeyEvent.META_ALT_LEFT_ON;
      case KeyEvent.KEYCODE_ALT_RIGHT -> KeyEvent.META_ALT_RIGHT_ON;
      case KeyEvent.KEYCODE_SHIFT_LEFT -> KeyEvent.META_SHIFT_LEFT_ON;
      case KeyEvent.KEYCODE_SHIFT_RIGHT -> KeyEvent.META_SHIFT_RIGHT_ON;
      case KeyEvent.KEYCODE_SYM -> KeyEvent.META_SYM_ON;
      case KeyEvent.KEYCODE_FUNCTION -> KeyEvent.META_FUNCTION_ON;
      case KeyEvent.KEYCODE_CTRL_LEFT -> KeyEvent.META_CTRL_LEFT_ON;
      case KeyEvent.KEYCODE_CTRL_RIGHT -> KeyEvent.META_CTRL_RIGHT_ON;
      case KeyEvent.KEYCODE_META_LEFT -> KeyEvent.META_META_LEFT_ON;
      case KeyEvent.KEYCODE_META_RIGHT -> KeyEvent.META_META_RIGHT_ON;
      case KeyEvent.KEYCODE_CAPS_LOCK, KeyEvent.KEYCODE_NUM_LOCK, KeyEvent.KEYCODE_SCROLL_LOCK -> 0;
      default -> -1;
    };
    if (mask == -1) return old;
    if (mask == 0) {
      if (down) return old;
      int lock = keyCode == KeyEvent.KEYCODE_CAPS_LOCK ? KeyEvent.META_CAPS_LOCK_ON
          : keyCode == KeyEvent.KEYCODE_NUM_LOCK ? KeyEvent.META_NUM_LOCK_ON : KeyEvent.META_SCROLL_LOCK_ON;
      return old ^ lock;
    }
    int updated = down ? old | mask : old & ~(mask | KeyEvent.META_ALT_ON | KeyEvent.META_SHIFT_ON
        | KeyEvent.META_CTRL_ON | KeyEvent.META_META_ON);
    return KeyEvent.normalizeMetaState(updated);
  }

  /** False for keys whose label is a space, separator, control or format character (or no label). */
  public boolean isPrintingKey(int keyCode) {
    return switch (Character.getType(getDisplayLabel(keyCode))) {
      case Character.SPACE_SEPARATOR, Character.LINE_SEPARATOR, Character.PARAGRAPH_SEPARATOR,
          Character.CONTROL, Character.FORMAT -> false;
      default -> true;
    };
  }
  public int getKeyboardType() {
    return FULL;
  }
  public int getModifierBehavior() {
    return switch (getKeyboardType()) {
      case FULL, SPECIAL_FUNCTION -> MODIFIER_BEHAVIOR_CHORDED;
      default -> MODIFIER_BEHAVIOR_CHORDED_OR_TOGGLED;
    };
  }
  /** Whether any input device has a physical key for the code; the virtual keyboard has none. */
  public static boolean deviceHasKey(int keyCode) {
    return deviceHasKeys(new int[] {keyCode})[0];
  }
  public static boolean[] deviceHasKeys(int[] keyCodes) {
    return new boolean[keyCodes.length];
  }

  @Override public int describeContents() { return 0; }
  @Override public void writeToParcel(Parcel out, int flags) {
    if (out == null) throw new IllegalArgumentException("parcel must not be null");
    out.writeInt(deviceId);
  }
  public static final Parcelable.Creator<KeyCharacterMap> CREATOR = new Parcelable.Creator<>() {
    @Override public KeyCharacterMap createFromParcel(Parcel in) {
      int id = in.readInt();
      return id == VIRTUAL_KEYBOARD ? VIRTUAL : new KeyCharacterMap(id);
    }
    @Override public KeyCharacterMap[] newArray(int size) { return new KeyCharacterMap[size]; }
  };

  public static class UnavailableException extends AndroidRuntimeException {
    public UnavailableException(String msg) {
      super(msg);
    }
  }

  // AOSP's accent tables: combining marks and their spacing accents, plus one-way legacy aliases.
  private static final SparseIntArray COMBINING_TO_ACCENT = new SparseIntArray();
  private static final SparseIntArray ACCENT_TO_COMBINING = new SparseIntArray();
  private static final SparseIntArray DEAD_KEY_CACHE = new SparseIntArray();
  private static void addCombining(int combining, int accent) {
    COMBINING_TO_ACCENT.append(combining, accent);
    ACCENT_TO_COMBINING.append(accent, combining);
  }
  static {
    addCombining(0x0300, 0x02CB); // Grave
    addCombining(0x0301, 0x02CA); // Acute
    addCombining(0x0302, 0x02C6); // Circumflex
    addCombining(0x0303, 0x02DC); // Tilde
    addCombining(0x0304, 0x02C9); // Macron
    addCombining(0x0306, 0x02D8); // Breve
    addCombining(0x0307, 0x02D9); // Dot above
    addCombining(0x0308, 0x00A8); // Diaeresis
    addCombining(0x0309, 0x02C0); // Hook above
    addCombining(0x030A, 0x02DA); // Ring above
    addCombining(0x030B, 0x02DD); // Double acute
    addCombining(0x030C, 0x02C7); // Caron
    addCombining(0x030D, 0x02C8); // Vertical line above
    addCombining(0x030E, 0x0022); // Double vertical line above
    addCombining(0x0313, 0x02BC); // Comma above
    addCombining(0x0314, 0x02BD); // Reversed comma above
    addCombining(0x0315, 0x02B9); // Comma above right
    addCombining(0x031B, 0x02BA); // Horn
    addCombining(0x031C, 0x02D3); // Left half ring below
    addCombining(0x031D, 0x02D4); // Up tack below
    addCombining(0x031E, 0x02D5); // Down tack below
    addCombining(0x031F, 0x02D6); // Plus sign below
    addCombining(0x0320, 0x02D7); // Minus sign below
    addCombining(0x0323, 0x02F3); // Dot below
    addCombining(0x0326, 0x02CF); // Comma below
    addCombining(0x0327, 0x00B8); // Cedilla
    addCombining(0x0328, 0x02DB); // Ogonek
    addCombining(0x0329, 0x02CC); // Vertical line below
    addCombining(0x032D, 0x02EF); // Circumflex below
    addCombining(0x032E, 0x02F0); // Breve below
    addCombining(0x0330, 0x02F7); // Tilde below
    addCombining(0x0331, 0x02CD); // Macron below
    addCombining(0x0332, 0x02CD); // Low line
    addCombining(0x0334, 0x02F7); // Tilde overlay
    addCombining(0x0339, 0x02D3); // Right half ring below
    addCombining(0x033A, 0x02EE); // Inverted bridge below
    addCombining(0x033F, 0x02BF); // Double overline
    addCombining(0x0345, 0x037A); // Ypogegrammeni
    // One-way mappings to equivalent preferred accents.
    COMBINING_TO_ACCENT.append(0x0340, 0x02CB); // Grave tone mark
    COMBINING_TO_ACCENT.append(0x0341, 0x02CA); // Acute tone mark
    COMBINING_TO_ACCENT.append(0x0343, 0x02BC); // Greek koronis
    COMBINING_TO_ACCENT.append(0x0344, 0x0385); // Greek dialytika tonos
    // One-way legacy mappings to preserve compatibility with older applications.
    ACCENT_TO_COMBINING.append(0x0060, 0x0300); // Grave
    ACCENT_TO_COMBINING.append(0x005E, 0x0302); // Circumflex
    ACCENT_TO_COMBINING.append(0x0027, 0x0301); // Acute
    ACCENT_TO_COMBINING.append(0x00B4, 0x0301); // Acute
    ACCENT_TO_COMBINING.append(0x007E, 0x0303); // Tilde
    ACCENT_TO_COMBINING.append(0x00AF, 0x0304); // Macron
  }
}
