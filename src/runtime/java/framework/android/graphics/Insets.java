package android.graphics;
public final class Insets {
  public final int left, top, right, bottom;
  public static final Insets NONE = new Insets(0, 0, 0, 0);
  private Insets(int l, int t, int r, int b) {
    left = l;
    top = t;
    right = r;
    bottom = b;
  }
  public static Insets of(int l, int t, int r, int b) {
    return (l | t | r | b) == 0 ? NONE : new Insets(l, t, r, b);
  }
  public static Insets of(Rect r) {
    return of(r.left, r.top, r.right, r.bottom);
  }
  public boolean equals(Object value) {
    return value instanceof Insets i && left == i.left && top == i.top && right == i.right
        && bottom == i.bottom;
  }
  public int hashCode() {
    return java.util.Objects.hash(left, top, right, bottom);
  }
}
