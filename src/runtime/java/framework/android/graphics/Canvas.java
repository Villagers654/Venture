package android.graphics;
// There is no host view renderer: drawing is accepted and discarded, and the
// canvas keeps only its size and save depth.
public class Canvas {
  private int width, height, depth = 1;
  public Canvas() {}
  public Canvas(Bitmap bitmap) {
    setBitmap(bitmap);
  }
  public void setBitmap(Bitmap bitmap) {
    width = bitmap == null ? 0 : bitmap.getWidth();
    height = bitmap == null ? 0 : bitmap.getHeight();
  }
  public int getWidth() {
    return width;
  }
  public int getHeight() {
    return height;
  }
  public boolean isHardwareAccelerated() {
    return false;
  }
  public int save() {
    return depth++;
  }
  public int getSaveCount() {
    return depth;
  }
  public void restore() {
    if (depth <= 1) throw new IllegalStateException("Underflow in restore");
    --depth;
  }
  public void restoreToCount(int count) {
    if (count < 1 || count > depth) throw new IllegalArgumentException("Bad save count " + count);
    depth = count;
  }
  public void translate(float dx, float dy) {}
  public void scale(float sx, float sy) {}
  public void rotate(float degrees) {}
  public boolean clipRect(Rect rect) {
    return true;
  }
  public boolean clipRect(RectF rect) {
    return true;
  }
  public boolean clipRect(float left, float top, float right, float bottom) {
    return true;
  }
  public boolean clipRect(int left, int top, int right, int bottom) {
    return true;
  }
  public void drawColor(int color) {}
  public void drawRect(Rect rect, Paint paint) {}
  public void drawRect(RectF rect, Paint paint) {}
  public void drawRect(float left, float top, float right, float bottom, Paint paint) {}
  public void drawBitmap(Bitmap bitmap, float left, float top, Paint paint) {}
  public void drawBitmap(Bitmap bitmap, Rect src, Rect dst, Paint paint) {}
  public void drawBitmap(Bitmap bitmap, Rect src, RectF dst, Paint paint) {}
  public void drawText(String text, float x, float y, Paint paint) {}
  public void drawText(CharSequence text, int start, int end, float x, float y, Paint paint) {}
  public void drawText(char[] text, int index, int count, float x, float y, Paint paint) {}
}
