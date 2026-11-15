package android.graphics;
/** Shape outlines are recorded only: no shadow or clip renderer is installed. */
public final class Outline {
  private final Rect bounds = new Rect();
  private float radius, alpha = 1;
  private Path path;  // a convex path outline; null for empty and (round) rects
  public Outline() {}
  public Outline(Outline src) { set(src); }
  public void set(Outline src) {
    bounds.set(src.bounds);
    radius = src.radius;
    alpha = src.alpha;
    path = src.path == null ? null : new Path(src.path);
  }
  public void setRect(int left, int top, int right, int bottom) { setRoundRect(left, top, right, bottom, 0); }
  public void setRect(Rect rect) { setRect(rect.left, rect.top, rect.right, rect.bottom); }
  // AOSP: an empty rectangle makes an empty outline.
  public void setRoundRect(int left, int top, int right, int bottom, float radius) {
    if (left >= right || top >= bottom) {
      setEmpty();
      return;
    }
    bounds.set(left, top, right, bottom);
    this.radius = radius;
    path = null;
  }
  public void setRoundRect(Rect rect, float radius) { setRoundRect(rect.left, rect.top, rect.right, rect.bottom, radius); }
  // AOSP: a circle is a round rect of half its size; any other oval is a convex path.
  public void setOval(int left, int top, int right, int bottom) {
    if (left >= right || top >= bottom) {
      setEmpty();
      return;
    }
    if (right - left == bottom - top) {
      setRoundRect(left, top, right, bottom, (right - left) / 2.0f);
      return;
    }
    Path oval = new Path();
    oval.addOval(left, top, right, bottom, Path.Direction.CW);
    setConvexPath(oval);
  }
  public void setOval(Rect rect) { setOval(rect.left, rect.top, rect.right, rect.bottom); }
  public void setConvexPath(Path convexPath) {
    if (convexPath.isEmpty()) {
      setEmpty();
      return;
    }
    RectF r = new RectF();
    convexPath.computeBounds(r, false);
    bounds.set((int) Math.floor(r.left), (int) Math.floor(r.top), (int) Math.ceil(r.right), (int) Math.ceil(r.bottom));
    radius = 0;
    path = new Path(convexPath);
  }
  public void setEmpty() { bounds.set(0, 0, 0, 0); radius = 0; path = null; }
  public boolean isEmpty() { return path == null && bounds.isEmpty(); }
  // Only rect and round rect outlines clip, as on Android.
  public boolean canClip() { return path == null; }
  public void setAlpha(float alpha) { this.alpha = alpha; }
  public float getAlpha() { return alpha; }
  public float getRadius() { return radius; }
  public boolean getRect(Rect out) {
    if (path != null || bounds.isEmpty()) return false;
    out.set(bounds);
    return true;
  }
}
