package android.webkit;

/** AOSP android.webkit.WebResourceError (API 23): a WebViewClient.ERROR_* code and its description. */
public abstract class WebResourceError {
  WebResourceError() {}
  public abstract int getErrorCode();
  public abstract CharSequence getDescription();
}
