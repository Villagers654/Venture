package android.graphics.drawable;

import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Paint;
import android.graphics.PixelFormat;

public class BitmapDrawable extends Drawable {
  private final android.graphics.Bitmap bitmap;
  private final Paint paint = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.DITHER_FLAG);
  public BitmapDrawable(android.content.res.Resources resources, android.graphics.Bitmap bitmap) {
    this.bitmap = bitmap;
  }
  public android.graphics.Bitmap getBitmap() {
    return bitmap;
  }
  public final Paint getPaint() {
    return paint;
  }
  public int getIntrinsicWidth() {
    return bitmap.getWidth();
  }
  public int getIntrinsicHeight() {
    return bitmap.getHeight();
  }
  private int gravity = 119;
  public void setGravity(int gravity) { this.gravity = gravity; }
  public int getGravity() { return gravity; }
  // Gravity other than FILL is not laid out: the bitmap fills the bounds.
  public void draw(Canvas canvas) {
    if (bitmap != null && !getBounds().isEmpty()) canvas.drawBitmap(bitmap, null, getBounds(), paint);
  }
  public void setAlpha(int alpha) {
    if (alpha != paint.getAlpha()) {
      paint.setAlpha(alpha);
      invalidateSelf();
    }
  }
  public int getAlpha() {
    return paint.getAlpha();
  }
  public void setColorFilter(ColorFilter filter) {
    paint.setColorFilter(filter);
    invalidateSelf();
  }
  public ColorFilter getColorFilter() {
    return paint.getColorFilter();
  }
  public int getOpacity() {
    return PixelFormat.TRANSLUCENT;
  }
}
