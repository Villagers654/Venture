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
  private final int action, code;
  private final long downTime, eventTime;
  private final int repeat, meta, device, scan, flags, source;
  public KeyEvent(int a, int c) {
    this(android.os.SystemClock.uptimeMillis(), android.os.SystemClock.uptimeMillis(), a, c, 0);
  }
  public KeyEvent(long down, long event, int action, int code, int repeat) {
    this(down, event, action, code, repeat, 0, -1, 0, 0, 0x101);
  }
  public KeyEvent(long down, long event, int action, int code, int repeat, int meta,
      int device, int scan, int flags, int source) {
    this.downTime = down; this.eventTime = event; this.action = action; this.code = code;
    this.repeat = repeat; this.meta = meta; this.device = device; this.scan = scan;
    this.flags = flags; this.source = source;
  }
  public long getDownTime() { return downTime; }
  public int getMetaState() { return meta; }
  public int getModifiers() { return normalizeMetaState(meta) & 0x770ff; }
  public int getScanCode() { return scan; }
  public int getFlags() { return flags; }
  public static int normalizeMetaState(int state) {
    if ((state & 0xc0) != 0) state |= 1;
    if ((state & 0x30) != 0) state |= 2;
    if ((state & 0x6000) != 0) state |= 0x1000;
    if ((state & 0x60000) != 0) state |= 0x10000;
    return state;
  }
  public int getUnicodeChar() { return getUnicodeChar(meta); }
  public int getUnicodeChar(int state) {
    // The logical keyboard uses the framework's en-US default layout.
    boolean shift = (normalizeMetaState(state) & 1) != 0;
    if (code >= 29 && code <= 54)
      return ((shift ^ ((state & 0x100000) != 0)) ? 'A' : 'a') + code - 29;
    if (code >= 7 && code <= 16) return shift ? ")!@#$%^&*(".charAt(code - 7) : '0' + code - 7;
    return switch (code) { case 62 -> 32; case 61 -> 9; case 66 -> 10; case 67 -> 8; default -> 0; };
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
}
