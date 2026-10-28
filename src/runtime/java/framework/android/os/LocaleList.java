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
  public static LocaleList getDefault() {
    return new LocaleList(Locale.getDefault());
  }
  public String toLanguageTags() {
    return String.join(",", java.util.Arrays.stream(locales).map(Locale::toLanguageTag).toList());
  }
}
