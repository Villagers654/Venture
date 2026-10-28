package android.text;
public interface InputFilter {
  class LengthFilter implements InputFilter {
    private final int max;
    public LengthFilter(int m) {
      max = m;
    }
    public int getMax() {
      return max;
    }
    public CharSequence filter(
        CharSequence source, int start, int end, Spanned dest, int dstart, int dend) {
      int keep = max - (dest.length() - (dend - dstart));
      if (keep <= 0)
        return "";
      if (keep >= end - start)
        return null;
      if (Character.isHighSurrogate(source.charAt(start + keep - 1)))
        keep--;
      return source.subSequence(start, start + keep);
    }
  }
  CharSequence filter(CharSequence source, int start, int end, Spanned dest, int dstart, int dend);
}
