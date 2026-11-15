package android.graphics.drawable.shapes;

import android.graphics.Canvas;
import android.graphics.Outline;
import android.graphics.Paint;
import java.util.Objects;

// AOSP ArcShape: a pie wedge of the oval in the shape's size. Angles are degrees,
// clockwise from 3 o'clock.
public class ArcShape extends RectShape {
  private final float mStartAngle;
  private final float mSweepAngle;
  public ArcShape(float startAngle, float sweepAngle) {
    mStartAngle = startAngle;
    mSweepAngle = sweepAngle;
  }
  public final float getStartAngle() {
    return mStartAngle;
  }
  public final float getSweepAngle() {
    return mSweepAngle;
  }
  @Override
  public void draw(Canvas canvas, Paint paint) {
    canvas.drawArc(rect(), mStartAngle, mSweepAngle, true, paint);
  }
  // AOSP: outlines are convex only, so an arc provides none.
  @Override
  public void getOutline(Outline outline) {}
  @Override
  public ArcShape clone() throws CloneNotSupportedException {
    return (ArcShape) super.clone();
  }
  @Override
  public boolean equals(Object o) {
    if (this == o) return true;
    if (o == null || getClass() != o.getClass()) return false;
    if (!super.equals(o)) return false;
    ArcShape arcShape = (ArcShape) o;
    return Float.compare(arcShape.mStartAngle, mStartAngle) == 0
        && Float.compare(arcShape.mSweepAngle, mSweepAngle) == 0;
  }
  @Override
  public int hashCode() {
    return Objects.hash(super.hashCode(), mStartAngle, mSweepAngle);
  }
}
