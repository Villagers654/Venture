package android.graphics.drawable.shapes;

import android.graphics.Canvas;
import android.graphics.Outline;
import android.graphics.Paint;
import android.graphics.RectF;

// AOSP OvalShape: an ellipse inscribed in the shape's size.
public class OvalShape extends RectShape {
  public OvalShape() {}
  @Override
  public void draw(Canvas canvas, Paint paint) {
    canvas.drawOval(rect(), paint);
  }
  @Override
  public void getOutline(Outline outline) {
    final RectF rect = rect();
    outline.setOval((int) Math.ceil(rect.left), (int) Math.ceil(rect.top),
        (int) Math.floor(rect.right), (int) Math.floor(rect.bottom));
  }
  @Override
  public OvalShape clone() throws CloneNotSupportedException {
    return (OvalShape) super.clone();
  }
}
