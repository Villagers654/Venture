package android.graphics;

// AOSP graphics/java/android/graphics/Typeface.java: a font family and style.
// A typeface loaded from a font file carries that font, so a Canvas draws its text
// with the app's own glyphs; system families map to the host's logical fonts.
public class Typeface {
  public static final int NORMAL = 0, BOLD = 1, ITALIC = 2, BOLD_ITALIC = 3;
  public static final Typeface DEFAULT = new Typeface("sans-serif", NORMAL, null);
  public static final Typeface DEFAULT_BOLD = new Typeface("sans-serif", BOLD, null);
  public static final Typeface SANS_SERIF = new Typeface("sans-serif", NORMAL, null);
  public static final Typeface SERIF = new Typeface("serif", NORMAL, null);
  public static final Typeface MONOSPACE = new Typeface("monospace", NORMAL, null);
  private final String family;
  private final int style;
  private final java.awt.Font file;  // a font file's face, or null for a system family
  private Typeface(String family, int style, java.awt.Font file) {
    this.family = family;
    this.style = style;
    this.file = file;
  }
  public int getStyle() { return style; }
  public final boolean isBold() { return (style & BOLD) != 0; }
  public final boolean isItalic() { return (style & ITALIC) != 0; }
  public int getWeight() { return isBold() ? 700 : 400; }
  public static Typeface create(String familyName, int style) {
    return new Typeface(familyName == null ? "sans-serif" : familyName, style & BOLD_ITALIC, null);
  }
  public static Typeface create(Typeface family, int style) {
    return family == null ? create((String) null, style)
                          : new Typeface(family.family, style & BOLD_ITALIC, family.file);
  }
  public static Typeface create(Typeface family, int weight, boolean italic) {
    return create(family, (weight >= 600 ? BOLD : NORMAL) | (italic ? ITALIC : NORMAL));
  }
  public static Typeface defaultFromStyle(int style) { return create(DEFAULT, style); }
  // AOSP throws for a font it cannot read ("Font asset not found").
  public static Typeface createFromAsset(android.content.res.AssetManager assets, String path) {
    try (var in = assets.open(path)) {
      return new Typeface(path, NORMAL, read(in, path));
    } catch (java.io.IOException e) {
      throw new RuntimeException("Font asset not found " + path, e);
    }
  }
  public static Typeface createFromFile(String path) {
    try (var in = new java.io.FileInputStream(path)) {
      return new Typeface(path, NORMAL, read(in, path));
    } catch (java.io.IOException e) {
      throw new RuntimeException("Font not found " + path, e);
    }
  }
  public static Typeface createFromFile(java.io.File file) { return createFromFile(file.getPath()); }
  private static java.awt.Font read(java.io.InputStream in, String path) throws java.io.IOException {
    try {
      return java.awt.Font.createFont(java.awt.Font.TRUETYPE_FONT, in);
    } catch (java.awt.FontFormatException e) {
      // Android reads faces Java2D cannot (such as CFF outlines); draw those with
      // the default family rather than fail a font Android accepts.
      android.util.Log.w("Typeface", "Drawing " + path + " with the default family: " + e.getMessage());
      return null;
    }
  }
  // The host font this typeface draws with at a text size.
  java.awt.Font font(float size) {
    int awtStyle = (isBold() ? java.awt.Font.BOLD : 0) | (isItalic() ? java.awt.Font.ITALIC : 0);
    if (file != null) return file.deriveFont(awtStyle, size);
    String logical = switch (family) {
      case "serif" -> java.awt.Font.SERIF;
      case "monospace" -> java.awt.Font.MONOSPACED;
      default -> java.awt.Font.SANS_SERIF;
    };
    return new java.awt.Font(logical, awtStyle, 1).deriveFont(size);
  }
  @Override public boolean equals(Object o) {
    return o instanceof Typeface t && t.family.equals(family) && t.style == style;
  }
  @Override public int hashCode() { return family.hashCode() * 31 + style; }
}
