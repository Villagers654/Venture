package android.webkit;

/** A WebView's download callback (API 1); web plugins implement it in their own classes. */
public interface DownloadListener {
  void onDownloadStart(String url, String userAgent, String contentDisposition, String mimetype, long contentLength);
}
