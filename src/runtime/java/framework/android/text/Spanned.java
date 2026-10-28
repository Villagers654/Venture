package android.text;
public interface Spanned extends CharSequence {
  int SPAN_EXCLUSIVE_EXCLUSIVE = 33;
  <T> T[] getSpans(int start, int end, Class<T> type);
  int getSpanStart(Object tag);
  int getSpanEnd(Object tag);
  int getSpanFlags(Object tag);
  int nextSpanTransition(int start, int limit, Class type);
}
