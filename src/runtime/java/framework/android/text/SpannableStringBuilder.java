package android.text;
import java.util.*;

public class SpannableStringBuilder implements Editable {
  private final StringBuilder text = new StringBuilder();
  private final List<Span> spans = new ArrayList<>();
  private InputFilter[] filters = {};
  private static class Span {
    Object value;
    int start, end, flags;
    Span(Object v, int s, int e, int f) {
      value = v;
      start = s;
      end = e;
      flags = f;
    }
  }
  public SpannableStringBuilder() {}
  public SpannableStringBuilder(CharSequence source) {
    text.append(source);
  }
  public int length() {
    return text.length();
  }
  public char charAt(int i) {
    return text.charAt(i);
  }
  public CharSequence subSequence(int start, int end) {
    return text.substring(start, end);
  }
  public String toString() {
    return text.toString();
  }
  public Editable replace(int start, int end, CharSequence source, int from, int to) {
    Objects.checkFromToIndex(start, end, length());
    Objects.checkFromToIndex(from, to, source.length());
    CharSequence input = source.subSequence(from, to);
    for (InputFilter filter : filters) {
      CharSequence result = filter.filter(input, 0, input.length(), this, start, end);
      if (result != null)
        input = result;
    }
    int delta = input.length() - (end - start);
    // AOSP SpannableStringBuilder: watcher spans observe the old text first,
    // then the updated text and adjusted spans; afterTextChanged may edit again.
    TextWatcher[] watchers = getSpans(start, end, TextWatcher.class);
    int added = input.length(), removed = end - start;
    for (TextWatcher watcher : watchers) watcher.beforeTextChanged(this, start, removed, added);
    text.replace(start, end, input.toString());
    for (Span s : spans) {
      s.start = adjust(s.start, start, end, delta, (s.flags >> 4) & 0xf);
      s.end = adjust(s.end, start, end, delta, s.flags & 0xf);
    }
    for (TextWatcher watcher : watchers) watcher.onTextChanged(this, start, removed, added);
    for (TextWatcher watcher : watchers) watcher.afterTextChanged(this);
    return this;
  }
  private static int adjust(int p, int start, int end, int delta, int kind) {
    if (p < start) return p;
    if (p > end) return p + delta;
    if (start != end && p == end) return p + delta;
    return kind == 2 ? end + delta : start;
  }
  public Editable replace(int start, int end, CharSequence s) {
    return replace(start, end, s, 0, s.length());
  }
  public Editable insert(int where, CharSequence s) {
    return replace(where, where, s);
  }
  public Editable delete(int start, int end) {
    return replace(start, end, "");
  }
  public Editable append(CharSequence s) {
    return replace(length(), length(), String.valueOf(s));
  }
  public Editable append(CharSequence s, int start, int end) {
    return replace(length(), length(), s, start, end);
  }
  public Editable append(char c) {
    return append(String.valueOf(c));
  }
  public void clear() {
    replace(0, length(), "");
  }
  public void clearSpans() {
    spans.clear();
  }
  public void setFilters(InputFilter[] f) {
    filters = f.clone();
  }
  public InputFilter[] getFilters() {
    return filters.clone();
  }
  public void setSpan(Object what, int start, int end, int flags) {
    Objects.checkFromToIndex(start, end, length());
    removeSpan(what);
    spans.add(new Span(what, start, end, flags));
  }
  public void removeSpan(Object what) {
    spans.removeIf(s -> s.value == what);
  }
  @SuppressWarnings("unchecked")
  public <T> T[] getSpans(int start, int end, Class<T> type) {
    return spans.stream()
        .filter(s -> type.isInstance(s.value) && s.start <= end && s.end >= start)
        .map(s -> type.cast(s.value))
        .toArray(n -> (T[]) java.lang.reflect.Array.newInstance(type, n));
  }
  private Span span(Object value) {
    return spans.stream().filter(s -> s.value == value).findFirst().orElse(null);
  }
  public int getSpanStart(Object value) {
    Span s = span(value);
    return s == null ? -1 : s.start;
  }
  public int getSpanEnd(Object value) {
    Span s = span(value);
    return s == null ? -1 : s.end;
  }
  public int getSpanFlags(Object value) {
    Span s = span(value);
    return s == null ? 0 : s.flags;
  }
  public int nextSpanTransition(int start, int limit, Class type) {
    for (Span s : spans)
      if (type == null || type.isInstance(s.value)) {
        if (s.start > start)
          limit = Math.min(limit, s.start);
        if (s.end > start)
          limit = Math.min(limit, s.end);
      }
    return limit;
  }
}
