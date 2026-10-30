package android.app;

import android.os.LocaleList;

/**
 * AOSP android.app.LocaleManager (API 33): per-app language. The selection lasts for
 * the process; an empty list means the app follows the system locales.
 */
public class LocaleManager {
  private static LocaleList applicationLocales = LocaleList.getEmptyLocaleList();
  public LocaleManager(android.content.Context context) {}
  public LocaleList getApplicationLocales() { synchronized (LocaleManager.class) { return applicationLocales; } }
  public void setApplicationLocales(LocaleList locales) {
    java.util.Objects.requireNonNull(locales);
    synchronized (LocaleManager.class) { applicationLocales = locales; }
  }
  public LocaleList getSystemLocales() { return LocaleList.getDefault(); }
}
