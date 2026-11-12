package android.webkit;

import android.graphics.Bitmap;
import android.os.Message;
import android.view.KeyEvent;

/** AOSP android.webkit.WebViewClient: page load callbacks, with AOSP's default behaviour. */
public class WebViewClient {
  public static final int ERROR_UNKNOWN = -1, ERROR_HOST_LOOKUP = -2, ERROR_UNSUPPORTED_AUTH_SCHEME = -3,
      ERROR_AUTHENTICATION = -4, ERROR_PROXY_AUTHENTICATION = -5, ERROR_CONNECT = -6, ERROR_IO = -7,
      ERROR_TIMEOUT = -8, ERROR_REDIRECT_LOOP = -9, ERROR_UNSUPPORTED_SCHEME = -10,
      ERROR_FAILED_SSL_HANDSHAKE = -11, ERROR_BAD_URL = -12, ERROR_FILE = -13,
      ERROR_FILE_NOT_FOUND = -14, ERROR_TOO_MANY_REQUESTS = -15, ERROR_UNSAFE_RESOURCE = -16;
  public static final int SAFE_BROWSING_THREAT_UNKNOWN = 0, SAFE_BROWSING_THREAT_MALWARE = 1,
      SAFE_BROWSING_THREAT_PHISHING = 2, SAFE_BROWSING_THREAT_UNWANTED_SOFTWARE = 3,
      SAFE_BROWSING_THREAT_BILLING = 4;
  public WebViewClient() {}
  @Deprecated public boolean shouldOverrideUrlLoading(WebView view, String url) { return false; }
  public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
    return shouldOverrideUrlLoading(view, request.getUrl().toString());
  }
  public void onPageStarted(WebView view, String url, Bitmap favicon) {}
  public void onPageFinished(WebView view, String url) {}
  public void onLoadResource(WebView view, String url) {}
  public void onPageCommitVisible(WebView view, String url) {}
  @Deprecated public WebResourceResponse shouldInterceptRequest(WebView view, String url) { return null; }
  public WebResourceResponse shouldInterceptRequest(WebView view, WebResourceRequest request) {
    return shouldInterceptRequest(view, request.getUrl().toString());
  }
  @Deprecated public void onTooManyRedirects(WebView view, Message cancelMsg, Message continueMsg) {
    cancelMsg.sendToTarget();
  }
  @Deprecated public void onReceivedError(WebView view, int errorCode, String description, String failingUrl) {}
  /** AOSP: main-frame errors are also reported through the deprecated callback. */
  public void onReceivedError(WebView view, WebResourceRequest request, WebResourceError error) {
    if (request.isForMainFrame())
      onReceivedError(view, error.getErrorCode(), error.getDescription().toString(), request.getUrl().toString());
  }
  public void onReceivedHttpError(WebView view, WebResourceRequest request, WebResourceResponse errorResponse) {}
  public void onFormResubmission(WebView view, Message dontResend, Message resend) { dontResend.sendToTarget(); }
  public void doUpdateVisitedHistory(WebView view, String url, boolean isReload) {}
  public void onReceivedSslError(WebView view, SslErrorHandler handler, android.net.http.SslError error) {
    handler.cancel();
  }
  public void onReceivedHttpAuthRequest(WebView view, HttpAuthHandler handler, String host, String realm) {
    handler.cancel();
  }
  public boolean shouldOverrideKeyEvent(WebView view, KeyEvent event) { return false; }
  public void onUnhandledKeyEvent(WebView view, KeyEvent event) {}
  public void onScaleChanged(WebView view, float oldScale, float newScale) {}
  public void onReceivedLoginRequest(WebView view, String realm, String account, String args) {}
  public boolean onRenderProcessGone(WebView view, RenderProcessGoneDetail detail) { return false; }
}
