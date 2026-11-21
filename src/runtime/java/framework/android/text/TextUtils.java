package android.text;
public final class TextUtils {
  public enum TruncateAt { START, MIDDLE, END, MARQUEE, END_SMALL }
  public static final int CAP_MODE_CHARACTERS = 0x1000, CAP_MODE_WORDS = 0x2000, CAP_MODE_SENTENCES = 0x4000;
  /** AOSP: the length without leading and trailing characters at or below ' '. */
  public static int getTrimmedLength(CharSequence s) {
    int len = s.length(), start = 0;
    while (start < len && s.charAt(start) <= ' ') start++;
    int end = len;
    while (end > start && s.charAt(end - 1) <= ' ') end--;
    return end - start;
  }
  public static int indexOf(CharSequence s, char ch) { return indexOf(s, ch, 0); }
  public static int indexOf(CharSequence s, char ch, int start) {
    if (s instanceof String string) return string.indexOf(ch, start);
    return indexOf(s, ch, start, s.length());
  }
  public static int indexOf(CharSequence s, char ch, int start, int end) {
    for (int i = Math.max(start, 0); i < end; i++)
      if (s.charAt(i) == ch) return i;
    return -1;
  }
  public static int lastIndexOf(CharSequence s, char ch) { return lastIndexOf(s, ch, s.length() - 1); }
  public static int lastIndexOf(CharSequence s, char ch, int last) {
    if (s instanceof String string) return string.lastIndexOf(ch, last);
    return lastIndexOf(s, ch, 0, last);
  }
  public static int lastIndexOf(CharSequence s, char ch, int start, int last) {
    if (last < 0) return -1;
    if (last >= s.length()) last = s.length() - 1;
    for (int i = last; i >= Math.max(start, 0); i--)
      if (s.charAt(i) == ch) return i;
    return -1;
  }
  public static int indexOf(CharSequence s, CharSequence needle) { return indexOf(s, needle, 0, s.length()); }
  public static int indexOf(CharSequence s, CharSequence needle, int start) { return indexOf(s, needle, start, s.length()); }
  /** AOSP: the first match of needle at or after start that ends by end. */
  public static int indexOf(CharSequence s, CharSequence needle, int start, int end) {
    int length = needle.length();
    if (length == 0) return start;
    char first = needle.charAt(0);
    for (;;) {
      start = indexOf(s, first, start);
      if (start < 0 || start > end - length) return -1;
      if (regionMatches(s, start, needle, 0, length)) return start;
      start++;
    }
  }
  public static boolean regionMatches(CharSequence one, int toffset, CharSequence two, int ooffset, int len) {
    if (toffset < 0 || ooffset < 0 || toffset + len > one.length() || ooffset + len > two.length()) return false;
    for (int i = 0; i < len; i++)
      if (one.charAt(toffset + i) != two.charAt(ooffset + i)) return false;
    return true;
  }
  /** AOSP: the offset one character (a whole surrogate pair) before offset. */
  public static int getOffsetBefore(CharSequence text, int offset) {
    if (offset <= 1) return 0;
    char c = text.charAt(offset - 1);
    if (Character.isLowSurrogate(c) && Character.isHighSurrogate(text.charAt(offset - 2))) return offset - 2;
    return offset - 1;
  }
  /** AOSP: the offset one character (a whole surrogate pair) after offset. */
  public static int getOffsetAfter(CharSequence text, int offset) {
    int len = text.length();
    if (offset >= len - 1) return len;
    char c = text.charAt(offset);
    if (Character.isHighSurrogate(c) && Character.isLowSurrogate(text.charAt(offset + 1))) return offset + 2;
    return offset + 1;
  }
  /** AOSP: which requested CAP_MODE_* apply to text typed at off (start of a paragraph, word or sentence). */
  public static int getCapsMode(CharSequence cs, int off, int reqModes) {
    if (off < 0) return 0;
    int mode = (reqModes & CAP_MODE_CHARACTERS) != 0 ? CAP_MODE_CHARACTERS : 0;
    if ((reqModes & (CAP_MODE_WORDS | CAP_MODE_SENTENCES)) == 0) return mode;
    int i = off;
    char c;
    // Back over allowed opening punctuation.
    for (; i > 0; i--) {
      c = cs.charAt(i - 1);
      if (c != '"' && c != '\'' && Character.getType(c) != Character.START_PUNCTUATION) break;
    }
    // Start of paragraph, with optional whitespace.
    int j = i;
    while (j > 0 && ((c = cs.charAt(j - 1)) == ' ' || c == '\t')) j--;
    if (j == 0 || cs.charAt(j - 1) == '\n') return mode | CAP_MODE_WORDS | CAP_MODE_SENTENCES;
    if ((reqModes & CAP_MODE_SENTENCES) == 0) return i != j ? mode | CAP_MODE_WORDS : mode;
    if (i == j) return mode; // a sentence starts only after a space
    // Back over allowed closing punctuation.
    for (; j > 0; j--) {
      c = cs.charAt(j - 1);
      if (c != '"' && c != '\'' && Character.getType(c) != Character.END_PUNCTUATION) break;
    }
    if (j > 0) {
      c = cs.charAt(j - 1);
      if (c == '.' || c == '?' || c == '!') {
        // A period inside the word marks an abbreviation (e.g.), not a sentence end.
        if (c == '.')
          for (int k = j - 2; k >= 0; k--) {
            c = cs.charAt(k);
            if (c == '.') return mode;
            if (!Character.isLetter(c)) break;
          }
        return mode | CAP_MODE_SENTENCES;
      }
    }
    return mode;
  }
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
  /** Copies spans of {@code kind} overlapping [start, end) to {@code dest} at {@code destOffset}. */
  public static void copySpansFrom(Spanned source, int start, int end, Class kind, Spannable dest, int destOffset) {
    if (kind == null) kind = Object.class;
    for (Object span : source.getSpans(start, end, kind)) {
      int st = Math.max(source.getSpanStart(span), start), en = Math.min(source.getSpanEnd(span), end);
      dest.setSpan(span, st - start + destOffset, en - start + destOffset, source.getSpanFlags(span));
    }
  }
  /** View.LAYOUT_DIRECTION_RTL for a right-to-left script, else LTR; null and ROOT are LTR. */
  public static int getLayoutDirectionFromLocale(java.util.Locale locale) {
    if (locale == null || locale.equals(java.util.Locale.ROOT)) return android.util.LayoutDirection.LTR;
    String script = locale.getScript();
    if (script.isEmpty()) {
      script = switch (locale.getLanguage()) {
        case "ar", "fa", "ur", "ps", "sd", "ug", "ckb" -> "Arab";
        case "he", "iw", "yi", "ji" -> "Hebr";
        case "dv" -> "Thaa";
        case "syr" -> "Syrc";
        default -> "";
      };
    }
    return switch (script) {
      case "Arab", "Hebr", "Thaa", "Syrc", "Nkoo", "Adlm", "Rohg", "Mand", "Samr" -> android.util.LayoutDirection.RTL;
      default -> android.util.LayoutDirection.LTR;
    };
  }
}
