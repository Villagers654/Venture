package android.graphics;
public class Paint {
  private int color = 0xff000000;
  private float textSize = 16;
  private int flags;
  private java.awt.Font font = new java.awt.Font("SansSerif", 0, 16);
  public Paint() {}
  public Paint(int flags) {
    this.flags = flags;
  }
  public int getFlags() {
    return flags;
  }
  public void setFlags(int flags) {
    this.flags = flags;
  }
  public void setColor(int c) {
    color = c;
  }
  public int getColor() {
    return color;
  }
  public void setTextSize(float size) {
    textSize = size;
    font = font.deriveFont(size);
  }
  public float getTextSize() {
    return textSize;
  }
  public float measureText(String text) {
    return (float) font
        .getStringBounds(
            text, new java.awt.font.FontRenderContext(null, (flags & 1) != 0, (flags & 128) != 0))
        .getWidth();
  }
  public boolean hasGlyph(String text) {
    return font.canDisplayUpTo(text) < 0;
  }
  public static class FontMetricsInt {
    public int top, ascent, descent, bottom, leading;
  }
}
