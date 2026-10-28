package android.text;
public interface Editable extends Spannable, Appendable {
  // AOSP android/text/Editable.java: a replaceable buffer factory, used by
  // AppCompat/Emoji to supply their own SpannableStringBuilder subclass.
  class Factory {
    private static final Factory INSTANCE = new Factory();
    public static Factory getInstance() { return INSTANCE; }
    public Editable newEditable(CharSequence source) {
      return new SpannableStringBuilder(java.util.Objects.requireNonNull(source));
    }
  }
  Editable replace(int start, int end, CharSequence source, int from, int to);
  Editable replace(int start, int end, CharSequence text);
  Editable insert(int where, CharSequence text);
  Editable delete(int start, int end);
  Editable append(CharSequence text);
  Editable append(CharSequence text, int start, int end);
  Editable append(char text);
  void clear();
  void clearSpans();
  void setFilters(InputFilter[] filters);
  InputFilter[] getFilters();
}
