package android.graphics.drawable.shapes;

import android.graphics.Canvas;
import android.graphics.Outline;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import java.util.Arrays;
import java.util.Objects;

// AOSP RoundRectShape: a rectangle with eight corner radii (top-left, top-right,
// bottom-right, bottom-left x/y pairs) and an optional inset inner round rect hole.
public class RoundRectShape extends RectShape {
  private float[] mOuterRadii;
  private RectF mInset;
  private float[] mInnerRadii;
  private RectF mInnerRect;
  private Path mPath;
  public RoundRectShape(float[] outerRadii, RectF inset, float[] innerRadii) {
    if (outerRadii != null && outerRadii.length < 8)
      throw new ArrayIndexOutOfBoundsException("outer radii must have >= 8 values");
    if (innerRadii != null && innerRadii.length < 8)
      throw new ArrayIndexOutOfBoundsException("inner radii must have >= 8 values");
    mOuterRadii = outerRadii;
    mInset = inset;
    mInnerRadii = innerRadii;
    if (inset != null) mInnerRect = new RectF();
    mPath = new Path();
  }
  @Override
  public void draw(Canvas canvas, Paint paint) {
    canvas.drawPath(mPath, paint);
  }
  @Override
  public void getOutline(Outline outline) {
    if (mInnerRect != null) return;  // a hole has no convex outline
    float radius = 0;
    if (mOuterRadii != null) {
      radius = mOuterRadii[0];
      for (int i = 1; i < 8; i++) {
        if (mOuterRadii[i] != radius) {
          // A round rect with mixed radii is only a convex path.
          outline.setConvexPath(mPath);
          return;
        }
      }
    }
    final RectF rect = rect();
    outline.setRoundRect((int) Math.ceil(rect.left), (int) Math.ceil(rect.top),
        (int) Math.floor(rect.right), (int) Math.floor(rect.bottom), radius);
  }
  @Override
  protected void onResize(float w, float h) {
    super.onResize(w, h);
    RectF r = rect();
    mPath.reset();
    if (mOuterRadii != null) mPath.addRoundRect(r, mOuterRadii, Path.Direction.CW);
    else mPath.addRect(r, Path.Direction.CW);
    if (mInnerRect != null) {
      mInnerRect.set(r.left + mInset.left, r.top + mInset.top, r.right - mInset.right, r.bottom - mInset.bottom);
      if (mInnerRect.width() < w && mInnerRect.height() < h) {
        if (mInnerRadii != null) mPath.addRoundRect(mInnerRect, mInnerRadii, Path.Direction.CCW);
        else mPath.addRect(mInnerRect, Path.Direction.CCW);
      }
    }
  }
  @Override
  public RoundRectShape clone() throws CloneNotSupportedException {
    final RoundRectShape shape = (RoundRectShape) super.clone();
    shape.mOuterRadii = mOuterRadii != null ? mOuterRadii.clone() : null;
    shape.mInnerRadii = mInnerRadii != null ? mInnerRadii.clone() : null;
    shape.mInset = mInset != null ? new RectF(mInset) : null;
    shape.mInnerRect = mInnerRect != null ? new RectF(mInnerRect) : null;
    shape.mPath = new Path(mPath);
    return shape;
  }
  @Override
  public boolean equals(Object o) {
    if (this == o) return true;
    if (o == null || getClass() != o.getClass()) return false;
    if (!super.equals(o)) return false;
    RoundRectShape that = (RoundRectShape) o;
    return Arrays.equals(mOuterRadii, that.mOuterRadii) && Objects.equals(mInset, that.mInset)
        && Arrays.equals(mInnerRadii, that.mInnerRadii) && Objects.equals(mInnerRect, that.mInnerRect);
  }
  @Override
  public int hashCode() {
    int result = Objects.hash(super.hashCode(), mInset, mInnerRect);
    result = 31 * result + Arrays.hashCode(mOuterRadii);
    return 31 * result + Arrays.hashCode(mInnerRadii);
  }
}
