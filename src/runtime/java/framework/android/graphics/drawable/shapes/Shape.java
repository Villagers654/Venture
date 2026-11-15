package android.graphics.drawable.shapes;

import android.graphics.Canvas;
import android.graphics.Outline;
import android.graphics.Paint;
import java.util.Objects;

// AOSP graphics/java/android/graphics/drawable/shapes/Shape.java: a resizable
// geometric shape that draws itself with a Paint.
public abstract class Shape implements Cloneable {
  private float mWidth, mHeight;
  public Shape() {}
  public final float getWidth() {
    return mWidth;
  }
  public final float getHeight() {
    return mHeight;
  }
  public abstract void draw(Canvas canvas, Paint paint);
  public final void resize(float width, float height) {
    if (width < 0) width = 0;
    if (height < 0) height = 0;
    if (mWidth != width || mHeight != height) {
      mWidth = width;
      mHeight = height;
      onResize(width, height);
    }
  }
  public boolean hasAlpha() {
    return true;
  }
  protected void onResize(float width, float height) {}
  public void getOutline(Outline outline) {}
  @Override
  public Shape clone() throws CloneNotSupportedException {
    return (Shape) super.clone();
  }
  @Override
  public boolean equals(Object o) {
    if (this == o) return true;
    if (o == null || getClass() != o.getClass()) return false;
    Shape shape = (Shape) o;
    return Float.compare(shape.mWidth, mWidth) == 0 && Float.compare(shape.mHeight, mHeight) == 0;
  }
  @Override
  public int hashCode() {
    return Objects.hash(mWidth, mHeight);
  }
}
