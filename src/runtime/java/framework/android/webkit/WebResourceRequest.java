package android.webkit;

import java.util.Map;

/** AOSP android.webkit.WebResourceRequest (API 21). */
public interface WebResourceRequest {
  android.net.Uri getUrl();
  boolean isForMainFrame();
  default boolean isRedirect() { return false; }
  boolean hasGesture();
  String getMethod();
  Map<String, String> getRequestHeaders();
}
