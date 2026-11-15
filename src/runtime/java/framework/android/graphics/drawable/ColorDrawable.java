package android.graphics.drawable;

import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Paint;
import android.graphics.PixelFormat;

// AOSP ColorDrawable: the base color, modulated by the drawable alpha, fills the bounds.
public class ColorDrawable extends Drawable {
  private int baseColor, useColor;
  private ColorFilter colorFilter;
  private final Paint paint = new Paint();
  public ColorDrawable() {}
  public ColorDrawable(int c) {
    setColor(c);
  }
  public int getColor() {
    return useColor;
  }
  public void setColor(int c) {
    if (baseColor != c || useColor != c) {
      baseColor = useColor = c;
      invalidateSelf();
    }
  }
  public int getAlpha() {
    return useColor >>> 24;
  }
  public void setAlpha(int alpha) {
    alpha += alpha >> 7;  // make it 0..256
    int baseAlpha = baseColor >>> 24;
    int useAlpha = baseAlpha * alpha >> 8;
    int color = (baseColor << 8 >>> 8) | (useAlpha << 24);
    if (useColor != color) {
      useColor = color;
      invalidateSelf();
    }
  }
  public void setColorFilter(ColorFilter filter) {
    colorFilter = filter;
  }
  public ColorFilter getColorFilter() {
    return colorFilter;
  }
  public int getOpacity() {
    if (colorFilter != null) return PixelFormat.TRANSLUCENT;
    return switch (useColor >>> 24) {
      case 255 -> PixelFormat.OPAQUE;
      case 0 -> PixelFormat.TRANSPARENT;
      default -> PixelFormat.TRANSLUCENT;
    };
  }
  public void draw(Canvas canvas) {
    if ((useColor >>> 24) != 0 || colorFilter != null) {
      paint.setColor(useColor);
      paint.setColorFilter(colorFilter);
      canvas.drawRect(getBounds(), paint);
    }
  }
}
