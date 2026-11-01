package android.graphics;
/** Shape outlines are recorded only: no shadow or clip renderer is installed. */
public final class Outline {
  private final Rect bounds = new Rect();
  private float radius, alpha = 1;
  public void setRect(int left, int top, int right, int bottom) { bounds.set(left, top, right, bottom); radius = 0; }
  public void setRect(Rect rect) { setRect(rect.left, rect.top, rect.right, rect.bottom); }
  public void setRoundRect(int left, int top, int right, int bottom, float radius) {
    bounds.set(left, top, right, bottom); this.radius = radius;
  }
  public void setOval(int left, int top, int right, int bottom) { setRoundRect(left, top, right, bottom, 0); }
  public void setEmpty() { bounds.set(0, 0, 0, 0); radius = 0; }
  public boolean isEmpty() { return bounds.isEmpty(); }
  public void setAlpha(float alpha) { this.alpha = alpha; }
  public float getAlpha() { return alpha; }
  public float getRadius() { return radius; }
  public boolean getRect(Rect out) { out.set(bounds.left, bounds.top, bounds.right, bounds.bottom); return true; }
}
