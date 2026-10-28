package android.graphics.drawable;
public class BitmapDrawable extends Drawable {
  private final android.graphics.Bitmap bitmap;
  public BitmapDrawable(android.content.res.Resources resources, android.graphics.Bitmap bitmap) {
    this.bitmap = bitmap;
  }
  public android.graphics.Bitmap getBitmap() {
    return bitmap;
  }
  public int getIntrinsicWidth() {
    return bitmap.getWidth();
  }
  public int getIntrinsicHeight() {
    return bitmap.getHeight();
  }
}
