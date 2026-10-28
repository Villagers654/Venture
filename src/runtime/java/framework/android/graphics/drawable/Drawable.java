package android.graphics.drawable;
public abstract class Drawable {
  private int changingConfigurations;
  public int getChangingConfigurations() {
    return changingConfigurations;
  }
  public void setChangingConfigurations(int c) {
    changingConfigurations = c;
  }
  public Drawable mutate() {
    return this;
  }
  public Drawable getCurrent() {
    return this;
  }
  public ConstantState getConstantState() {
    return null;
  }
  public static abstract class ConstantState {
    public abstract Drawable newDrawable();
    public Drawable newDrawable(android.content.res.Resources r) {
      return newDrawable();
    }
    public abstract int getChangingConfigurations();
  }
  private final android.graphics.Rect bounds = new android.graphics.Rect();
  public void setBounds(int l, int t, int r, int b) {
    bounds.set(l, t, r, b);
  }
  public android.graphics.Rect getBounds() {
    return bounds;
  }
  public int getIntrinsicWidth() {
    return -1;
  }
  public int getIntrinsicHeight() {
    return -1;
  }
  public interface Callback {
    void invalidateDrawable(Drawable d);
    void scheduleDrawable(Drawable d, Runnable r, long when);
    void unscheduleDrawable(Drawable d, Runnable r);
  }
}
