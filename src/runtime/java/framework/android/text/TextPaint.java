package android.text;
public class TextPaint extends android.graphics.Paint {
  public int bgColor, baselineShift, linkColor;
  public float density = 1;
  public TextPaint() {}
  public TextPaint(int flags) {
    super(flags);
  }
}
