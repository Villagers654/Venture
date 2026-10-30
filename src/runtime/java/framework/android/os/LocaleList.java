package android.os;
import java.util.Locale;
public final class LocaleList {
  private final Locale[] locales;
  public LocaleList(Locale... values) {
    locales = values.clone();
  }
  public Locale get(int i) {
    return locales[i];
  }
  public int size() {
    return locales.length;
  }
  public boolean isEmpty() {
    return locales.length == 0;
  }
  private static LocaleList defaultList;
  public static synchronized LocaleList getDefault() {
    return defaultList != null ? defaultList : new LocaleList(Locale.getDefault());
  }
  public static LocaleList getAdjustedDefault() { return getDefault(); }
  public static LocaleList getEmptyLocaleList() { return new LocaleList(); }
  public static synchronized void setDefault(LocaleList locales) {
    if (locales == null) throw new NullPointerException("locales is null");
    if (locales.isEmpty()) throw new IllegalArgumentException("locales is empty");
    defaultList = locales;
    Locale.setDefault(locales.get(0));
  }
  public static LocaleList forLanguageTags(String list) {
    if (list == null || list.isEmpty()) return new LocaleList();
    return new LocaleList(java.util.Arrays.stream(list.split(",")).map(Locale::forLanguageTag).toArray(Locale[]::new));
  }
  public int indexOf(Locale locale) {
    for (int i = 0; i < locales.length; i++)
      if (locales[i].equals(locale)) return i;
    return -1;
  }
  @Override public boolean equals(Object other) {
    return other instanceof LocaleList list && java.util.Arrays.equals(locales, list.locales);
  }
  @Override public int hashCode() { return java.util.Arrays.hashCode(locales); }
  @Override public String toString() { return java.util.Arrays.toString(locales); }
  public String toLanguageTags() {
    return String.join(",", java.util.Arrays.stream(locales).map(Locale::toLanguageTag).toList());
  }
}
