package android.graphics;

// AOSP graphics/java/android/graphics/Typeface.java: a font family and style.
// Venture draws no Java views, so a typeface only carries its family and style.
public class Typeface {
  public static final int NORMAL = 0, BOLD = 1, ITALIC = 2, BOLD_ITALIC = 3;
  public static final Typeface DEFAULT = new Typeface("sans-serif", NORMAL);
  public static final Typeface DEFAULT_BOLD = new Typeface("sans-serif", BOLD);
  public static final Typeface SANS_SERIF = new Typeface("sans-serif", NORMAL);
  public static final Typeface SERIF = new Typeface("serif", NORMAL);
  public static final Typeface MONOSPACE = new Typeface("monospace", NORMAL);
  private final String family;
  private final int style;
  private Typeface(String family, int style) { this.family = family; this.style = style; }
  public int getStyle() { return style; }
  public final boolean isBold() { return (style & BOLD) != 0; }
  public final boolean isItalic() { return (style & ITALIC) != 0; }
  public int getWeight() { return isBold() ? 700 : 400; }
  public static Typeface create(String familyName, int style) {
    return new Typeface(familyName == null ? "sans-serif" : familyName, style & BOLD_ITALIC);
  }
  public static Typeface create(Typeface family, int style) {
    return new Typeface(family == null ? "sans-serif" : family.family, style & BOLD_ITALIC);
  }
  public static Typeface create(Typeface family, int weight, boolean italic) {
    return create(family, (weight >= 600 ? BOLD : NORMAL) | (italic ? ITALIC : NORMAL));
  }
  public static Typeface defaultFromStyle(int style) { return create(DEFAULT, style); }
  public static Typeface createFromAsset(android.content.res.AssetManager assets, String path) {
    return new Typeface(path, NORMAL);
  }
  public static Typeface createFromFile(String path) { return new Typeface(path, NORMAL); }
  public static Typeface createFromFile(java.io.File file) { return new Typeface(file.getPath(), NORMAL); }
  @Override public boolean equals(Object o) {
    return o instanceof Typeface t && t.family.equals(family) && t.style == style;
  }
  @Override public int hashCode() { return family.hashCode() * 31 + style; }
}
