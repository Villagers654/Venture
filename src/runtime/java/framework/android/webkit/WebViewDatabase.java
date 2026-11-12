package android.webkit;

import java.util.HashMap;
import java.util.Map;

/** AOSP android.webkit.WebViewDatabase: saved form data and HTTP auth credentials of WebViews. */
public abstract class WebViewDatabase {
  private static WebViewDatabase instance;
  public WebViewDatabase() {}
  public static synchronized WebViewDatabase getInstance(android.content.Context context) {
    if (instance == null) instance = new Memory();
    return instance;
  }
  @Deprecated public abstract boolean hasUsernamePassword();
  @Deprecated public abstract void clearUsernamePassword();
  public abstract boolean hasHttpAuthUsernamePassword();
  public abstract void clearHttpAuthUsernamePassword();
  public abstract void setHttpAuthUsernamePassword(String host, String realm, String username, String password);
  public abstract String[] getHttpAuthUsernamePassword(String host, String realm);
  @Deprecated public abstract boolean hasFormData();
  public abstract void clearFormData();

  // Session-only, like a WebView whose pages never submit forms.
  private static final class Memory extends WebViewDatabase {
    private final Map<String, String[]> auth = new HashMap<>();
    @Override public boolean hasUsernamePassword() { return false; }
    @Override public void clearUsernamePassword() {}
    @Override public synchronized boolean hasHttpAuthUsernamePassword() { return !auth.isEmpty(); }
    @Override public synchronized void clearHttpAuthUsernamePassword() { auth.clear(); }
    @Override public synchronized void setHttpAuthUsernamePassword(String host, String realm, String username, String password) {
      if (host != null && realm != null) auth.put(host + '\n' + realm, new String[] {username, password});
    }
    @Override public synchronized String[] getHttpAuthUsernamePassword(String host, String realm) {
      String[] value = auth.get(host + '\n' + realm);
      return value == null ? null : value.clone();
    }
    @Override public boolean hasFormData() { return false; }
    @Override public void clearFormData() {}
  }
}
