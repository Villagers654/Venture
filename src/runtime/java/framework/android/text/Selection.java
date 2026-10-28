package android.text;
public final class Selection {
  private static final Object START = new Object(), END = new Object();
  public static void setSelection(Spannable s, int start, int end) {
    s.setSpan(START, start, start, 34);
    s.setSpan(END, end, end, 34);
  }
  public static void setSelection(Spannable s, int pos) {
    setSelection(s, pos, pos);
  }
  public static int getSelectionStart(CharSequence s) {
    return s instanceof Spanned p ? p.getSpanStart(START) : -1;
  }
  public static int getSelectionEnd(CharSequence s) {
    return s instanceof Spanned p ? p.getSpanStart(END) : -1;
  }
}
