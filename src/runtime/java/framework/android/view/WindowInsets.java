package android.view;
public class WindowInsets {
  public static final WindowInsets CONSUMED = new WindowInsets((android.graphics.Rect) null);
  public static final class Type {
    public static int statusBars() {
      return 1;
    }
    public static int navigationBars() {
      return 2;
    }
    public static int captionBar() {
      return 4;
    }
    public static int ime() {
      return 8;
    }
    public static int systemGestures() {
      return 16;
    }
    public static int mandatorySystemGestures() {
      return 32;
    }
    public static int tappableElement() {
      return 64;
    }
    public static int all() {
      return 255;
    }
    public static int systemBars() {
      return 7;
    }
    public static int displayCutout() {
      return 128;
    }
  }
  private final android.graphics.Rect system;
  private boolean consumed;
  private android.graphics.Insets stable = android.graphics.Insets.NONE;
  private android.graphics.Insets[] typed;
  private android.graphics.Insets[] maximum;
  private int visible;
  private DisplayCutout cutout;
  private boolean round;
  public static final class Builder {
    private final WindowInsets value;
    public Builder() {
      value = new WindowInsets(new android.graphics.Rect());
    }
    public Builder(WindowInsets source) {
      value = new WindowInsets(source);
      value.consumed = false;
    }
    public Builder setSystemWindowInsets(android.graphics.Insets insets) {
      value.system.left = insets.left;
      value.system.top = insets.top;
      value.system.right = insets.right;
      value.system.bottom = insets.bottom;
      setInsets(Type.statusBars(), android.graphics.Insets.of(0, insets.top, 0, 0));
      setInsets(Type.navigationBars(),
          android.graphics.Insets.of(insets.left, 0, insets.right, insets.bottom));
      return this;
    }
    public Builder setStableInsets(android.graphics.Insets insets) {
      value.stable = insets;
      return this;
    }
    public Builder setSystemGestureInsets(android.graphics.Insets i) {
      return setInsets(16, i);
    }
    public Builder setMandatorySystemGestureInsets(android.graphics.Insets i) {
      return setInsets(32, i);
    }
    public Builder setTappableElementInsets(android.graphics.Insets i) {
      return setInsets(64, i);
    }
    public Builder setInsets(int mask, android.graphics.Insets insets) {
      if (value.typed == null)
        value.typed = initial(value.system);
      set(value.typed, mask, insets);
      var bars = value.getInsets(Type.systemBars());
      value.system.left = bars.left;
      value.system.top = bars.top;
      value.system.right = bars.right;
      value.system.bottom = bars.bottom;
      return this;
    }
    public Builder setInsetsIgnoringVisibility(int mask, android.graphics.Insets insets) {
      if ((mask & Type.ime()) != 0)
        throw new IllegalArgumentException("IME maximum insets are unavailable");
      if (value.maximum == null)
        value.maximum = initial(new android.graphics.Rect());
      set(value.maximum, mask, insets);
      return this;
    }
    public Builder setVisible(int mask, boolean visible) {
      value.visible = visible ? value.visible | mask : value.visible & ~mask;
      return this;
    }
    public Builder setDisplayCutout(DisplayCutout cutout) {
      value.cutout = cutout;
      var safe = cutout == null ? android.graphics.Insets.NONE : android.graphics.Insets.of(
          cutout.getSafeInsetLeft(), cutout.getSafeInsetTop(), cutout.getSafeInsetRight(), cutout.getSafeInsetBottom());
      setInsets(Type.displayCutout(), safe);
      setInsetsIgnoringVisibility(Type.displayCutout(), safe);
      setVisible(Type.displayCutout(), cutout != null);
      return this;
    }
    public Builder setRound(boolean round) {
      value.round = round;
      return this;
    }
    public WindowInsets build() {
      return new WindowInsets(value);
    }
    private static void set(
        android.graphics.Insets[] values, int mask, android.graphics.Insets insets) {
      java.util.Objects.requireNonNull(insets);
      for (int i = 0; i < 8; ++i)
        if ((mask & (1 << i)) != 0)
          values[i] = insets;
    }
  }
  private static android.graphics.Insets[] initial(android.graphics.Rect r) {
    var values = new android.graphics.Insets[8];
    java.util.Arrays.fill(values, android.graphics.Insets.NONE);
    values[0] = android.graphics.Insets.of(0, r.top, 0, 0);
    values[1] = android.graphics.Insets.of(r.left, 0, r.right, r.bottom);
    return values;
  }
  public WindowInsets(android.graphics.Rect r) {
    system = r == null ? new android.graphics.Rect() : new android.graphics.Rect(r);
    consumed = r == null;
  }
  public WindowInsets(WindowInsets i) {
    system = new android.graphics.Rect(i.system);
    consumed = i.consumed;
    stable = i.stable;
    typed = i.typed == null ? null : i.typed.clone();
    maximum = i.maximum == null ? null : i.maximum.clone();
    visible = i.visible;
    cutout = i.cutout;
    round = i.round;
  }
  public int getSystemWindowInsetLeft() {
    return system.left;
  }
  public int getSystemWindowInsetTop() {
    return system.top;
  }
  public int getSystemWindowInsetRight() {
    return system.right;
  }
  public int getSystemWindowInsetBottom() {
    return system.bottom;
  }
  public int getStableInsetTop() {
    return stable.top;
  }
  public int getStableInsetBottom() {
    return stable.bottom;
  }
  public int getStableInsetLeft() {
    return stable.left;
  }
  public int getStableInsetRight() {
    return stable.right;
  }
  public boolean isVisible(int mask) {
    return (visible & mask) == mask;
  }
  public boolean isConsumed() {
    return consumed;
  }
  public boolean isRound() {
    return round;
  }
  public WindowInsets consumeSystemWindowInsets() {
    WindowInsets copy = new WindowInsets(this);
    copy.system.set(0, 0, 0, 0);
    if (copy.typed != null)
      for (int i = 0; i < 3; ++i) copy.typed[i] = android.graphics.Insets.NONE;
    copy.consumed = !copy.hasInsets();
    return copy;
  }
  public WindowInsets consumeStableInsets() {
    WindowInsets copy = new WindowInsets(this);
    copy.stable = android.graphics.Insets.NONE;
    copy.consumed = !copy.hasInsets();
    return copy;
  }
  public WindowInsets consumeDisplayCutout() {
    WindowInsets copy = new WindowInsets(this);
    copy.cutout = null;
    if (copy.typed != null)
      copy.typed[7] = android.graphics.Insets.NONE;
    if (copy.maximum != null)
      copy.maximum[7] = android.graphics.Insets.NONE;
    copy.consumed = !copy.hasInsets();
    return copy;
  }
  public boolean hasSystemWindowInsets() {
    return nonzero(getInsets(Type.systemBars()));
  }
  public boolean hasStableInsets() {
    return nonzero(stable);
  }
  public boolean hasInsets() {
    return nonzero(getInsets(Type.all())) || hasStableInsets() || cutout != null;
  }
  private static boolean nonzero(android.graphics.Insets value) {
    return (value.left | value.top | value.right | value.bottom) != 0;
  }
  public WindowInsets replaceSystemWindowInsets(int left, int top, int right, int bottom) {
    return new Builder(this)
        .setSystemWindowInsets(android.graphics.Insets.of(left, top, right, bottom))
        .build();
  }
  public WindowInsets replaceSystemWindowInsets(android.graphics.Rect r) {
    return replaceSystemWindowInsets(r.left, r.top, r.right, r.bottom);
  }
  public android.graphics.Insets getInsets(int type) {
    if (typed != null)
      return combined(typed, type);
    return (type & Type.systemBars()) != 0
        ? android.graphics.Insets.of(system.left, system.top, system.right, system.bottom)
        : android.graphics.Insets.NONE;
  }
  private static android.graphics.Insets combined(android.graphics.Insets[] values, int mask) {
    int l = 0, t = 0, r = 0, b = 0;
    for (int i = 0; i < 8; ++i)
      if ((mask & (1 << i)) != 0) {
        var v = values[i];
        l = Math.max(l, v.left);
        t = Math.max(t, v.top);
        r = Math.max(r, v.right);
        b = Math.max(b, v.bottom);
      }
    return android.graphics.Insets.of(l, t, r, b);
  }
  public android.graphics.Insets getInsetsIgnoringVisibility(int mask) {
    if ((mask & Type.ime()) != 0)
      throw new IllegalArgumentException("IME maximum insets are unavailable");
    return maximum == null ? getInsets(mask) : combined(maximum, mask);
  }
  public android.graphics.Insets getSystemGestureInsets() {
    return getInsets(16);
  }
  public android.graphics.Insets getMandatorySystemGestureInsets() {
    return getInsets(32);
  }
  public android.graphics.Insets getTappableElementInsets() {
    return getInsets(64);
  }
  public DisplayCutout getDisplayCutout() {
    return cutout;
  }
}
