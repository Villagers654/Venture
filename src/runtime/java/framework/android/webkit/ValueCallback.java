package android.webkit;

/** Callback for asynchronous WebView values (cookies, JavaScript results). */
public interface ValueCallback<T> {
  void onReceiveValue(T value);
}
