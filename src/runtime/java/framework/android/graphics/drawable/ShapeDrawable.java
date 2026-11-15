package android.graphics.drawable;

import android.content.res.ColorStateList;
import android.content.res.Resources;
import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Outline;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.PorterDuff;
import android.graphics.PorterDuffColorFilter;
import android.graphics.Rect;
import android.graphics.Shader;
import android.graphics.drawable.shapes.Shape;

// AOSP graphics/java/android/graphics/drawable/ShapeDrawable.java: draws its Shape,
// resized to the bounds, with its Paint; without a shape it fills the bounds.
public class ShapeDrawable extends Drawable {
  private ShapeState mShapeState;
  private PorterDuffColorFilter mTintFilter;
  private boolean mMutated;
  public ShapeDrawable() {
    this(new ShapeState(), null);
  }
  public ShapeDrawable(Shape s) {
    this(new ShapeState(), null);
    mShapeState.mShape = s;
  }
  private ShapeDrawable(ShapeState state, Resources res) {
    mShapeState = state;
    updateLocalState();
  }
  public Shape getShape() {
    return mShapeState.mShape;
  }
  public void setShape(Shape s) {
    mShapeState.mShape = s;
    updateShape();
  }
  public void setShaderFactory(ShaderFactory fact) {
    mShapeState.mShaderFactory = fact;
  }
  public ShaderFactory getShaderFactory() {
    return mShapeState.mShaderFactory;
  }
  public Paint getPaint() {
    return mShapeState.mPaint;
  }
  public void setPadding(int left, int top, int right, int bottom) {
    if ((left | top | right | bottom) == 0) {
      mShapeState.mPadding = null;
    } else {
      if (mShapeState.mPadding == null) mShapeState.mPadding = new Rect();
      mShapeState.mPadding.set(left, top, right, bottom);
    }
    invalidateSelf();
  }
  public void setPadding(Rect padding) {
    if (padding == null) {
      mShapeState.mPadding = null;
    } else {
      if (mShapeState.mPadding == null) mShapeState.mPadding = new Rect();
      mShapeState.mPadding.set(padding);
    }
    invalidateSelf();
  }
  public void setIntrinsicWidth(int width) {
    mShapeState.mIntrinsicWidth = width;
    invalidateSelf();
  }
  public void setIntrinsicHeight(int height) {
    mShapeState.mIntrinsicHeight = height;
    invalidateSelf();
  }
  @Override
  public int getIntrinsicWidth() {
    return mShapeState.mIntrinsicWidth;
  }
  @Override
  public int getIntrinsicHeight() {
    return mShapeState.mIntrinsicHeight;
  }
  @Override
  public boolean getPadding(Rect padding) {
    if (mShapeState.mPadding != null) {
      padding.set(mShapeState.mPadding);
      return true;
    }
    return super.getPadding(padding);
  }
  private static int modulateAlpha(int paintAlpha, int alpha) {
    int scale = alpha + (alpha >>> 7);  // convert to 0..256
    return paintAlpha * scale >>> 8;
  }
  protected void onDraw(Shape shape, Canvas canvas, Paint paint) {
    shape.draw(canvas, paint);
  }
  @Override
  public void draw(Canvas canvas) {
    final Rect r = getBounds();
    final ShapeState state = mShapeState;
    final Paint paint = state.mPaint;
    final int prevAlpha = paint.getAlpha();
    paint.setAlpha(modulateAlpha(prevAlpha, state.mAlpha));
    if (paint.getAlpha() != 0) {
      final boolean clearColorFilter;
      if (mTintFilter != null && paint.getColorFilter() == null) {
        paint.setColorFilter(mTintFilter);
        clearColorFilter = true;
      } else {
        clearColorFilter = false;
      }
      if (state.mShape != null) {
        final int count = canvas.save();
        canvas.translate(r.left, r.top);
        onDraw(state.mShape, canvas, paint);
        canvas.restoreToCount(count);
      } else {
        canvas.drawRect(r, paint);
      }
      if (clearColorFilter) paint.setColorFilter(null);
    }
    paint.setAlpha(prevAlpha);
  }
  @Override
  public int getChangingConfigurations() {
    return super.getChangingConfigurations() | mShapeState.getChangingConfigurations();
  }
  @Override
  public void setAlpha(int alpha) {
    mShapeState.mAlpha = alpha;
    invalidateSelf();
  }
  @Override
  public int getAlpha() {
    return mShapeState.mAlpha;
  }
  @Override
  public void setTintList(ColorStateList tint) {
    mShapeState.mTint = tint;
    mTintFilter = updateTintFilter(mTintFilter, tint, mShapeState.mTintMode);
    invalidateSelf();
  }
  @Override
  public void setTintMode(PorterDuff.Mode tintMode) {
    mShapeState.mTintMode = tintMode;
    mTintFilter = updateTintFilter(mTintFilter, mShapeState.mTint, tintMode);
    invalidateSelf();
  }
  @Override
  public void setColorFilter(ColorFilter colorFilter) {
    mShapeState.mPaint.setColorFilter(colorFilter);
    invalidateSelf();
  }
  @Override
  public ColorFilter getColorFilter() {
    return mShapeState.mPaint.getColorFilter();
  }
  @Override
  public int getOpacity() {
    if (mShapeState.mShape == null) {
      final int alpha = mShapeState.mPaint.getAlpha();
      if (alpha == 0) return PixelFormat.TRANSPARENT;
      if (alpha == 255) return PixelFormat.OPAQUE;
    }
    // Not sure, so be safe.
    return PixelFormat.TRANSLUCENT;
  }
  @Override
  public void setDither(boolean dither) {
    mShapeState.mPaint.setDither(dither);
    invalidateSelf();
  }
  @Override
  protected void onBoundsChange(Rect bounds) {
    super.onBoundsChange(bounds);
    updateShape();
  }
  @Override
  protected boolean onStateChange(int[] stateSet) {
    final ShapeState state = mShapeState;
    if (state.mTint != null && state.mTintMode != null) {
      mTintFilter = updateTintFilter(mTintFilter, state.mTint, state.mTintMode);
      return true;
    }
    return false;
  }
  @Override
  public boolean isStateful() {
    final ShapeState s = mShapeState;
    return super.isStateful() || (s.mTint != null && s.mTint.isStateful());
  }
  private void updateShape() {
    if (mShapeState.mShape != null) {
      final Rect r = getBounds();
      final int w = r.width();
      final int h = r.height();
      mShapeState.mShape.resize(w, h);
      if (mShapeState.mShaderFactory != null) mShapeState.mPaint.setShader(mShapeState.mShaderFactory.resize(w, h));
    }
    invalidateSelf();
  }
  @Override
  public void getOutline(Outline outline) {
    if (mShapeState.mShape != null) {
      mShapeState.mShape.getOutline(outline);
      outline.setAlpha(getAlpha() / 255.0f);
    }
  }
  @Override
  public ConstantState getConstantState() {
    mShapeState.mChangingConfigurations = getChangingConfigurations();
    return mShapeState;
  }
  @Override
  public Drawable mutate() {
    if (!mMutated && super.mutate() == this) {
      mShapeState = new ShapeState(mShapeState);
      updateLocalState();
      mMutated = true;
    }
    return this;
  }
  @Override
  public void clearMutated() {
    super.clearMutated();
    mMutated = false;
  }
  private void updateLocalState() {
    mTintFilter = updateTintFilter(mTintFilter, mShapeState.mTint, mShapeState.mTintMode);
  }
  static final class ShapeState extends ConstantState {
    final Paint mPaint;
    int mChangingConfigurations;
    Shape mShape;
    ColorStateList mTint;
    PorterDuff.Mode mTintMode = DEFAULT_TINT_MODE;
    Rect mPadding;
    int mIntrinsicWidth;
    int mIntrinsicHeight;
    int mAlpha = 255;
    ShaderFactory mShaderFactory;
    ShapeState() {
      mPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    }
    ShapeState(ShapeState orig) {
      mChangingConfigurations = orig.mChangingConfigurations;
      mPaint = new Paint(orig.mPaint);
      if (orig.mShape != null) {
        try {
          mShape = orig.mShape.clone();
        } catch (CloneNotSupportedException e) {
          // Well, at least we tried.
          mShape = orig.mShape;
        }
      }
      mTint = orig.mTint;
      mTintMode = orig.mTintMode;
      if (orig.mPadding != null) mPadding = new Rect(orig.mPadding);
      mIntrinsicWidth = orig.mIntrinsicWidth;
      mIntrinsicHeight = orig.mIntrinsicHeight;
      mAlpha = orig.mAlpha;
      mShaderFactory = orig.mShaderFactory;
    }
    @Override
    public Drawable newDrawable() {
      return new ShapeDrawable(new ShapeState(this), null);
    }
    @Override
    public Drawable newDrawable(Resources res) {
      return new ShapeDrawable(new ShapeState(this), res);
    }
    @Override
    public int getChangingConfigurations() {
      return mChangingConfigurations;
    }
  }
  // Supplies the paint's shader each time the drawable is resized.
  public static abstract class ShaderFactory {
    public ShaderFactory() {}
    public abstract Shader resize(int width, int height);
  }
}
