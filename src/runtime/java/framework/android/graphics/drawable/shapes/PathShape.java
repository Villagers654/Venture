package android.graphics.drawable.shapes;

import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import java.util.Objects;

// AOSP PathShape: a path in a standard width x height space, scaled to the shape's size.
public class PathShape extends Shape {
  private final float mStdWidth;
  private final float mStdHeight;
  private Path mPath;
  private float mScaleX = 1;
  private float mScaleY = 1;
  public PathShape(Path path, float stdWidth, float stdHeight) {
    mPath = path;
    mStdWidth = stdWidth;
    mStdHeight = stdHeight;
  }
  @Override
  public void draw(Canvas canvas, Paint paint) {
    canvas.save();
    canvas.scale(mScaleX, mScaleY);
    canvas.drawPath(mPath, paint);
    canvas.restore();
  }
  @Override
  protected void onResize(float width, float height) {
    mScaleX = width / mStdWidth;
    mScaleY = height / mStdHeight;
  }
  @Override
  public PathShape clone() throws CloneNotSupportedException {
    final PathShape shape = (PathShape) super.clone();
    shape.mPath = new Path(mPath);
    return shape;
  }
  @Override
  public boolean equals(Object o) {
    if (this == o) return true;
    if (o == null || getClass() != o.getClass()) return false;
    if (!super.equals(o)) return false;
    PathShape pathShape = (PathShape) o;
    return Float.compare(pathShape.mStdWidth, mStdWidth) == 0
        && Float.compare(pathShape.mStdHeight, mStdHeight) == 0
        && Float.compare(pathShape.mScaleX, mScaleX) == 0
        && Float.compare(pathShape.mScaleY, mScaleY) == 0
        && Objects.equals(mPath, pathShape.mPath);
  }
  @Override
  public int hashCode() {
    return Objects.hash(super.hashCode(), mStdWidth, mStdHeight, mPath, mScaleX, mScaleY);
  }
}
