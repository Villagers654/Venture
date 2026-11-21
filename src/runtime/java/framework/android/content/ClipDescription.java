package android.content;
// A clip's label and MIME types (the 2D framework's TextView reads it to offer Paste).
public class ClipDescription {
  public static final String MIMETYPE_TEXT_PLAIN = "text/plain";
  private final CharSequence label;
  private final String[] mimeTypes;
  public ClipDescription(CharSequence label, String[] mimeTypes) {
    if (mimeTypes == null) throw new NullPointerException("mimeTypes is null");
    this.label = label;
    this.mimeTypes = mimeTypes.clone();
  }
  public CharSequence getLabel() {
    return label;
  }
  public int getMimeTypeCount() {
    return mimeTypes.length;
  }
  public String getMimeType(int index) {
    return mimeTypes[index];
  }
  public boolean hasMimeType(String type) {
    for (String mime : mimeTypes)
      if (compareMimeTypes(mime, type)) return true;
    return false;
  }
  public static boolean compareMimeTypes(String concrete, String desired) {
    if (concrete.equals(desired) || desired.equals("*/*")) return true;
    int slash = desired.indexOf('/');
    return desired.endsWith("/*") && concrete.regionMatches(0, desired, 0, slash + 1);
  }
}
