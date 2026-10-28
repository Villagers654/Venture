package android.graphics.drawable;
public class ColorDrawable extends Drawable {
  private int color;
  public ColorDrawable() {}
  public ColorDrawable(int c) {
    color = c;
  }
  public int getColor() {
    return color;
  }
  public void setColor(int c) {
    color = c;
  }
}
