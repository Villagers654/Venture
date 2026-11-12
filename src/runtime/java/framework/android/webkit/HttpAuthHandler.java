package android.webkit;

/** AOSP android.webkit.HttpAuthHandler: answers an HTTP authentication request of a page load. */
public class HttpAuthHandler extends android.os.Handler {
  public HttpAuthHandler() { super(android.os.Looper.getMainLooper()); }
  public boolean useHttpAuthUsernamePassword() { return false; }
  public void cancel() {}
  public void proceed(String username, String password) {}
}
