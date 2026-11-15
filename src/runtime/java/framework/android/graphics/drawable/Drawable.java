package android.graphics.drawable;

import android.content.res.ColorStateList;
import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Insets;
import android.graphics.Outline;
import android.graphics.PixelFormat;
import android.graphics.PorterDuff;
import android.graphics.PorterDuffColorFilter;
import android.graphics.Rect;

// AOSP graphics/java/android/graphics/drawable/Drawable.java: bounds, state, level,
// visibility, callback and tint bookkeeping. Subclasses draw into a Canvas; a Canvas
// without a bitmap discards the drawing (there is no host view renderer).
public abstract class Drawable {
  private static final int[] WILD_CARD = new int[0];
  static final PorterDuff.Mode DEFAULT_TINT_MODE = PorterDuff.Mode.SRC_IN;
  private int[] stateSet = WILD_CARD;
  private int level;
  private int changingConfigurations;
  private boolean visible = true;
  private int layoutDirection;
  private java.lang.ref.WeakReference<Callback> callback;
  private final Rect bounds = new Rect();
  public Drawable() {}
  public abstract void draw(Canvas canvas);
  public void setBounds(int l, int t, int r, int b) {
    if (bounds.left != l || bounds.top != t || bounds.right != r || bounds.bottom != b) {
      if (!bounds.isEmpty()) invalidateSelf();
      bounds.set(l, t, r, b);
      onBoundsChange(bounds);
    }
  }
  public void setBounds(Rect r) {
    setBounds(r.left, r.top, r.right, r.bottom);
  }
  public final void copyBounds(Rect out) {
    out.set(bounds);
  }
  public final Rect copyBounds() {
    return new Rect(bounds);
  }
  public final Rect getBounds() {
    return bounds;
  }
  public Rect getDirtyBounds() {
    return getBounds();
  }
  public int getChangingConfigurations() {
    return changingConfigurations;
  }
  public void setChangingConfigurations(int c) {
    changingConfigurations = c;
  }
  @Deprecated
  public void setDither(boolean dither) {}
  public void setFilterBitmap(boolean filter) {}
  public boolean isFilterBitmap() {
    return false;
  }
  public final void setCallback(Callback cb) {
    callback = cb == null ? null : new java.lang.ref.WeakReference<>(cb);
  }
  public Callback getCallback() {
    return callback == null ? null : callback.get();
  }
  public void invalidateSelf() {
    Callback cb = getCallback();
    if (cb != null) cb.invalidateDrawable(this);
  }
  public void scheduleSelf(Runnable what, long when) {
    Callback cb = getCallback();
    if (cb != null) cb.scheduleDrawable(this, what, when);
  }
  public void unscheduleSelf(Runnable what) {
    Callback cb = getCallback();
    if (cb != null) cb.unscheduleDrawable(this, what);
  }
  public int getLayoutDirection() {
    return layoutDirection;
  }
  public final boolean setLayoutDirection(int direction) {
    if (layoutDirection != direction) {
      layoutDirection = direction;
      return onLayoutDirectionChanged(direction);
    }
    return false;
  }
  public boolean onLayoutDirectionChanged(int direction) {
    return false;
  }
  public abstract void setAlpha(int alpha);
  public int getAlpha() {
    return 0xFF;
  }
  public abstract void setColorFilter(ColorFilter colorFilter);
  @Deprecated
  public void setColorFilter(int color, PorterDuff.Mode mode) {
    if (getColorFilter() instanceof PorterDuffColorFilter filter && filter.color == color && filter.mode == mode)
      return;
    setColorFilter(new PorterDuffColorFilter(color, mode));
  }
  public ColorFilter getColorFilter() {
    return null;
  }
  public void clearColorFilter() {
    setColorFilter(null);
  }
  public void setTint(int tintColor) {
    setTintList(ColorStateList.valueOf(tintColor));
  }
  public void setTintList(ColorStateList tint) {}
  public void setTintMode(PorterDuff.Mode tintMode) {}
  public void setHotspot(float x, float y) {}
  public void setHotspotBounds(int left, int top, int right, int bottom) {}
  public void getHotspotBounds(Rect outRect) {
    outRect.set(getBounds());
  }
  public boolean isProjected() {
    return false;
  }
  public boolean isStateful() {
    return false;
  }
  public boolean hasFocusStateSpecified() {
    return false;
  }
  public boolean setState(int[] stateSet) {
    if (!java.util.Arrays.equals(this.stateSet, stateSet)) {
      this.stateSet = stateSet;
      return onStateChange(stateSet);
    }
    return false;
  }
  public int[] getState() {
    return stateSet;
  }
  public void jumpToCurrentState() {}
  public Drawable getCurrent() {
    return this;
  }
  public final boolean setLevel(int level) {
    if (this.level != level) {
      this.level = level;
      return onLevelChange(level);
    }
    return false;
  }
  public final int getLevel() {
    return level;
  }
  public boolean setVisible(boolean visible, boolean restart) {
    boolean changed = this.visible != visible;
    if (changed) {
      this.visible = visible;
      invalidateSelf();
    }
    return changed;
  }
  public final boolean isVisible() {
    return visible;
  }
  public void setAutoMirrored(boolean mirrored) {}
  public boolean isAutoMirrored() {
    return false;
  }
  public boolean canApplyTheme() {
    return false;
  }
  public void applyTheme(android.content.res.Resources.Theme t) {}
  @Deprecated
  public abstract int getOpacity();
  public static int resolveOpacity(int op1, int op2) {
    if (op1 == op2) return op1;
    if (op1 == PixelFormat.UNKNOWN || op2 == PixelFormat.UNKNOWN) return PixelFormat.UNKNOWN;
    if (op1 == PixelFormat.TRANSLUCENT || op2 == PixelFormat.TRANSLUCENT) return PixelFormat.TRANSLUCENT;
    if (op1 == PixelFormat.TRANSPARENT || op2 == PixelFormat.TRANSPARENT) return PixelFormat.TRANSPARENT;
    return PixelFormat.OPAQUE;
  }
  protected boolean onStateChange(int[] state) {
    return false;
  }
  protected boolean onLevelChange(int level) {
    return false;
  }
  protected void onBoundsChange(Rect bounds) {}
  public int getIntrinsicWidth() {
    return -1;
  }
  public int getIntrinsicHeight() {
    return -1;
  }
  public int getMinimumWidth() {
    int width = getIntrinsicWidth();
    return width > 0 ? width : 0;
  }
  public int getMinimumHeight() {
    int height = getIntrinsicHeight();
    return height > 0 ? height : 0;
  }
  public boolean getPadding(Rect padding) {
    padding.set(0, 0, 0, 0);
    return false;
  }
  public Insets getOpticalInsets() {
    return Insets.NONE;
  }
  public void getOutline(Outline outline) {
    outline.setRect(getBounds());
    outline.setAlpha(0);
  }
  public Drawable mutate() {
    return this;
  }
  public void clearMutated() {}
  public ConstantState getConstantState() {
    return null;
  }
  // The tint filter for the current state, reusing the old one when it still matches.
  PorterDuffColorFilter updateTintFilter(PorterDuffColorFilter filter, ColorStateList tint, PorterDuff.Mode mode) {
    if (tint == null || mode == null) return null;
    int color = tint.getColorForState(getState(), 0);
    if (filter == null || filter.color != color || filter.mode != mode) return new PorterDuffColorFilter(color, mode);
    return filter;
  }
  public static abstract class ConstantState {
    public abstract Drawable newDrawable();
    public Drawable newDrawable(android.content.res.Resources r) {
      return newDrawable();
    }
    public Drawable newDrawable(android.content.res.Resources r, android.content.res.Resources.Theme theme) {
      return newDrawable(r);
    }
    public abstract int getChangingConfigurations();
    public boolean canApplyTheme() {
      return false;
    }
  }
  public interface Callback {
    void invalidateDrawable(Drawable d);
    void scheduleDrawable(Drawable d, Runnable r, long when);
    void unscheduleDrawable(Drawable d, Runnable r);
  }
}
