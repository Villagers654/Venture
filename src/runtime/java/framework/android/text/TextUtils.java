package android.text;
public final class TextUtils {
  public static boolean isEmpty(CharSequence s) {
    return s == null || s.length() == 0;
  }
  public static boolean equals(CharSequence a, CharSequence b) {
    return a == b || a != null && b != null && a.toString().contentEquals(b);
  }
  public static String join(CharSequence delimiter, Iterable<?> tokens) {
    StringBuilder b = new StringBuilder();
    for (Object t : tokens) {
      if (b.length() > 0)
        b.append(delimiter);
      b.append(t);
    }
    return b.toString();
  }
  public static String join(CharSequence delimiter, Object[] tokens) {
    return join(delimiter, java.util.Arrays.asList(tokens));
  }
  public static String[] split(String text, String regex) {
    return text.isEmpty() ? new String[0] : text.split(regex, -1);
  }
}
