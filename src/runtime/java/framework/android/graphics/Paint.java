package android.graphics;
public class Paint {
  public static final int ANTI_ALIAS_FLAG = 1, FILTER_BITMAP_FLAG = 2, DITHER_FLAG = 4,
      UNDERLINE_TEXT_FLAG = 8, STRIKE_THRU_TEXT_FLAG = 16, FAKE_BOLD_TEXT_FLAG = 32,
      LINEAR_TEXT_FLAG = 64, SUBPIXEL_TEXT_FLAG = 128;
  public enum Style { FILL, STROKE, FILL_AND_STROKE }
  public enum Align { LEFT, CENTER, RIGHT }
  public enum Cap { BUTT, ROUND, SQUARE }
  public enum Join { MITER, ROUND, BEVEL }
  private int color = 0xff000000;
  private float textSize = 16;
  private int flags;
  private Typeface typeface;
  private java.awt.Font font = Typeface.DEFAULT.font(16);
  private Style style = Style.FILL;
  private Align align = Align.LEFT;
  private Cap cap = Cap.BUTT;
  private Join join = Join.MITER;
  public Paint() {}
  public Paint(int flags) {
    this.flags = flags;
  }
  public Paint(Paint source) {
    set(source);
  }
  public void set(Paint source) {
    color = source.color;
    textSize = source.textSize;
    flags = source.flags;
    typeface = source.typeface;
    font = source.font;
    style = source.style;
    align = source.align;
    cap = source.cap;
    join = source.join;
    strokeWidth = source.strokeWidth;
  }
  public int getFlags() {
    return flags;
  }
  public void setFlags(int flags) {
    this.flags = flags;
  }
  public boolean isAntiAlias() { return (flags & ANTI_ALIAS_FLAG) != 0; }
  public void setAntiAlias(boolean on) { flags = on ? flags | ANTI_ALIAS_FLAG : flags & ~ANTI_ALIAS_FLAG; }
  public void setColor(int c) {
    color = c;
  }
  public int getColor() {
    return color;
  }
  public int getAlpha() { return color >>> 24; }
  public void setAlpha(int a) { color = (color & 0xffffff) | ((a & 255) << 24); }
  public void setTextSize(float size) {
    textSize = size;
    font = font.deriveFont(size);
  }
  public float getTextSize() {
    return textSize;
  }
  public Typeface getTypeface() { return typeface; }
  public Typeface setTypeface(Typeface typeface) {
    this.typeface = typeface;
    font = (typeface == null ? Typeface.DEFAULT : typeface).font(textSize);
    return typeface;
  }
  public Style getStyle() { return style; }
  public void setStyle(Style style) { this.style = style; }
  public Align getTextAlign() { return align; }
  public void setTextAlign(Align align) { this.align = align; }
  public Cap getStrokeCap() { return cap; }
  public void setStrokeCap(Cap cap) { this.cap = cap; }
  public Join getStrokeJoin() { return join; }
  public void setStrokeJoin(Join join) { this.join = join; }
  public float getTextScaleX() { return 1; }
  public float getTextSkewX() { return 0; }
  public float getLetterSpacing() { return 0; }
  public boolean isElegantTextHeight() { return false; }
  public String getFontFeatureSettings() { return null; }
  public String getFontVariationSettings() { return null; }
  java.awt.Font font() { return font; }
  // Java2D needs the host's font configuration (fontconfig on Linux). Without it every
  // font call throws, so text is measured with Roboto's proportions and not drawn.
  static final boolean HOST_FONTS = hostFonts();
  private static boolean hostFonts() {
    try {
      new java.awt.Font(java.awt.Font.SANS_SERIF, java.awt.Font.PLAIN, 12)
          .getLineMetrics("Ag", new java.awt.font.FontRenderContext(null, false, false));
      return true;
    } catch (Throwable unavailable) {
      return false;
    }
  }
  private float size() { return font.getSize2D(); }
  java.awt.font.FontRenderContext renderContext() {
    return new java.awt.font.FontRenderContext(null, (flags & ANTI_ALIAS_FLAG) != 0, (flags & SUBPIXEL_TEXT_FLAG) != 0);
  }
  public float measureText(String text) {
    if (!HOST_FONTS) return 0.55f * size() * text.length();
    return (float) font.getStringBounds(text, renderContext()).getWidth();
  }
  public float measureText(CharSequence text, int start, int end) {
    return measureText(text.subSequence(start, end).toString());
  }
  public float measureText(char[] text, int index, int count) {
    return measureText(new String(text, index, count));
  }
  public void getTextBounds(String text, int start, int end, Rect bounds) {
    if (!HOST_FONTS) {
      bounds.set(0, (int) Math.floor(-0.72f * size()), (int) Math.ceil(measureText(text.substring(start, end))), 0);
      return;
    }
    var r = new java.awt.font.TextLayout(text.substring(start, end).isEmpty() ? " " : text.substring(start, end),
        font, renderContext()).getBounds();
    bounds.set((int) Math.floor(r.getMinX()), (int) Math.floor(r.getMinY()),
        (int) Math.ceil(r.getMaxX()), (int) Math.ceil(r.getMaxY()));
  }
  public boolean hasGlyph(String text) {
    return !HOST_FONTS || font.canDisplayUpTo(text) < 0;
  }
  // Android's metrics are y-down from the baseline: ascent and top are negative.
  public static class FontMetrics {
    public float top, ascent, descent, bottom, leading;
  }
  public static class FontMetricsInt {
    public int top, ascent, descent, bottom, leading;
  }
  public FontMetrics getFontMetrics() {
    var metrics = new FontMetrics();
    getFontMetrics(metrics);
    return metrics;
  }
  public float getFontMetrics(FontMetrics metrics) {
    if (!HOST_FONTS) {
      float s = size(), ascent = -0.928f * s, descent = 0.244f * s;
      if (metrics != null) {
        metrics.ascent = ascent;
        metrics.descent = descent;
        metrics.leading = 0;
        metrics.top = -1.056f * s;
        metrics.bottom = 0.271f * s;
      }
      return descent - ascent;
    }
    var frc = renderContext();
    var line = font.getLineMetrics("Ag", frc);
    var max = font.getMaxCharBounds(frc);
    float ascent = -line.getAscent(), descent = line.getDescent();
    if (metrics != null) {
      metrics.ascent = ascent;
      metrics.descent = descent;
      metrics.leading = line.getLeading();
      metrics.top = Math.min(ascent, (float) max.getMinY());
      metrics.bottom = Math.max(descent, (float) max.getMaxY());
    }
    return descent - ascent + line.getLeading();
  }
  public FontMetricsInt getFontMetricsInt() {
    var metrics = new FontMetricsInt();
    getFontMetricsInt(metrics);
    return metrics;
  }
  public int getFontMetricsInt(FontMetricsInt metrics) {
    var f = getFontMetrics();
    if (metrics != null) {
      metrics.top = (int) Math.floor(f.top);
      metrics.ascent = Math.round(f.ascent);
      metrics.descent = Math.round(f.descent);
      metrics.bottom = (int) Math.ceil(f.bottom);
      metrics.leading = Math.round(f.leading);
    }
    return Math.round(f.descent - f.ascent + f.leading);
  }
  public float getFontSpacing() { return getFontMetrics(null); }
  private float strokeWidth;
  public void setStrokeWidth(float width) { strokeWidth = width; }
  public float getStrokeWidth() { return strokeWidth; }
  public void setARGB(int a, int r, int g, int b) { setColor((a << 24) | (r << 16) | (g << 8) | b); }
}
