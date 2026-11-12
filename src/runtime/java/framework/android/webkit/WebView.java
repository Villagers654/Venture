package android.webkit;

import android.content.Context;
import android.graphics.Bitmap;
import android.net.Uri;
import android.os.Message;
import android.util.AttributeSet;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * AOSP android.webkit.WebView without a web engine. Venture has no Chromium
 * renderer, so pages draw nothing, but the view keeps WebView's contract for
 * the apps that embed one (in-game web panels, sign-in helpers): navigation
 * state and back/forward history, settings, the client callback sequence on the
 * UI thread, JavaScript bridges, find and download listeners, and destroy().
 *
 * Every load commits a history entry and reports onPageStarted, progress and
 * onPageFinished like Chromium. about:, data: and loadData documents load (they
 * are blank here anyway). Any other URL cannot be fetched and rendered, so it
 * also reports a main-frame onReceivedError, as a device whose WebView failed
 * the request would; apps then show their own fallback instead of waiting.
 */
public class WebView extends android.widget.AbsoluteLayout {
  public static final int RENDERER_PRIORITY_WAIVED = 0, RENDERER_PRIORITY_BOUND = 1,
      RENDERER_PRIORITY_IMPORTANT = 2;
  public static final String SCHEME_TEL = "tel:", SCHEME_MAILTO = "mailto:", SCHEME_GEO = "geo:0,0?q=";

  public interface FindListener {
    void onFindResultReceived(int activeMatchOrdinal, int numberOfMatches, boolean isDoneCounting);
  }
  public abstract static class VisualStateCallback {
    public abstract void onComplete(long requestId);
  }
  public class WebViewTransport {
    private WebView webview;
    public synchronized void setWebView(WebView webview) { this.webview = webview; }
    public synchronized WebView getWebView() { return webview; }
  }
  public static class HitTestResult {
    public static final int UNKNOWN_TYPE = 0, ANCHOR_TYPE = 1, PHONE_TYPE = 2, GEO_TYPE = 3,
        EMAIL_TYPE = 4, IMAGE_TYPE = 5, IMAGE_ANCHOR_TYPE = 6, SRC_ANCHOR_TYPE = 7,
        SRC_IMAGE_ANCHOR_TYPE = 8, EDIT_TEXT_TYPE = 9;
    private int type = UNKNOWN_TYPE;
    private String extra;
    public HitTestResult() {}
    public void setType(int type) { this.type = type; }
    public void setExtra(String extra) { this.extra = extra; }
    public int getType() { return type; }
    public String getExtra() { return extra; }
  }

  private static boolean webContentsDebugging, rendererUnavailableReported;
  private static String dataDirectorySuffix;
  public static void setWebContentsDebuggingEnabled(boolean enabled) { webContentsDebugging = enabled; }
  public static synchronized void setDataDirectorySuffix(String suffix) {
    if (suffix.indexOf('/') >= 0) throw new IllegalArgumentException("Suffix " + suffix + " contains a path separator");
    dataDirectorySuffix = suffix;
  }
  public static void disableWebView() {}
  public static void clearClientCertPreferences(Runnable onCleared) {
    if (onCleared != null) new android.os.Handler(android.os.Looper.getMainLooper()).post(onCleared);
  }
  /** Safe Browsing needs Google Play Services, which a Quest does not have. */
  public static void startSafeBrowsing(Context context, ValueCallback<Boolean> callback) {
    if (callback != null) new android.os.Handler(android.os.Looper.getMainLooper()).post(() -> callback.onReceiveValue(false));
  }
  public static void setSafeBrowsingWhitelist(List<String> hosts, ValueCallback<Boolean> callback) {
    if (callback != null) callback.onReceiveValue(true);
  }
  public static Uri getSafeBrowsingPrivacyPolicyUrl() {
    return Uri.parse("https://www.google.com/chrome/browser/privacy/");
  }
  @Deprecated public static String findAddress(String addr) { return null; }
  public static ClassLoader getWebViewClassLoader() { return WebView.class.getClassLoader(); }

  private record Entry(String url, String originalUrl, String title) {}
  private final List<Entry> history = new ArrayList<>();
  private int index = -1, progress = 100, navigation;
  private final WebSettings settings = new WebSettings() {};
  private WebViewClient client = new WebViewClient();
  private WebChromeClient chromeClient;
  private DownloadListener downloadListener;
  private FindListener findListener;
  private final Map<String, Object> javascriptInterfaces = new HashMap<>();
  private boolean destroyed, paused, networkAvailable = true;
  private float scale = 1f;
  private int initialScale, rendererPriority = RENDERER_PRIORITY_IMPORTANT;
  private boolean rendererWaivedWhenNotVisible;
  private String findQuery;
  boolean acceptThirdPartyCookies;

  public WebView(Context context) { this(context, null); }
  public WebView(Context context, AttributeSet attrs) { this(context, attrs, 0); }
  public WebView(Context context, AttributeSet attrs, int defStyleAttr) { this(context, attrs, defStyleAttr, 0); }
  public WebView(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
    super(context, attrs, defStyleAttr, defStyleRes);
    if (context == null) throw new IllegalArgumentException("Invalid context argument");
    setFocusable(true);
    setFocusableInTouchMode(true);
  }
  @Deprecated public WebView(Context context, AttributeSet attrs, int defStyleAttr, boolean privateBrowsing) {
    this(context, attrs, defStyleAttr, 0);
  }

  public WebSettings getSettings() { return settings; }
  public void setWebViewClient(WebViewClient client) { this.client = client == null ? new WebViewClient() : client; }
  public WebViewClient getWebViewClient() { return client; }
  public void setWebChromeClient(WebChromeClient client) { chromeClient = client; }
  public WebChromeClient getWebChromeClient() { return chromeClient; }
  public void setDownloadListener(DownloadListener listener) { downloadListener = listener; }
  public void setFindListener(FindListener listener) { findListener = listener; }

  // Navigation.
  public void loadUrl(String url, Map<String, String> additionalHttpHeaders) { loadUrl(url); }
  public void loadUrl(String url) {
    if (destroyed || url == null) return;
    if (URLUtil.isJavaScriptUrl(url)) return; // javascript: runs in the current page; there is no script engine
    navigate(url, url, isLocalDocument(url));
  }
  public void postUrl(String url, byte[] postData) {
    if (destroyed || url == null) return;
    if (URLUtil.isNetworkUrl(url)) navigate(url, url, false); else loadUrl(url);
  }
  public void loadData(String data, String mimeType, String encoding) {
    if (destroyed) return;
    navigate("data:" + (mimeType == null ? "text/html" : mimeType) + ("base64".equals(encoding) ? ";base64" : "")
        + "," + (data == null ? "" : data), null, true);
  }
  public void loadDataWithBaseURL(String baseUrl, String data, String mimeType, String encoding, String historyUrl) {
    if (destroyed) return;
    String url = baseUrl == null || URLUtil.isDataUrl(baseUrl) ? "about:blank" : baseUrl;
    navigate(url, historyUrl == null || historyUrl.isEmpty() ? "about:blank" : historyUrl, true);
  }
  public void reload() {
    if (destroyed || index < 0) return;
    Entry e = history.get(index);
    start(e.url, isLocalDocument(e.url) || URLUtil.isDataUrl(e.url), true);
  }
  public void stopLoading() { if (!destroyed) ++navigation; }
  public boolean canGoBack() { return canGoBackOrForward(-1); }
  public void goBack() { goBackOrForward(-1); }
  public boolean canGoForward() { return canGoBackOrForward(1); }
  public void goForward() { goBackOrForward(1); }
  public boolean canGoBackOrForward(int steps) {
    int target = index + steps;
    return !destroyed && target >= 0 && target < history.size() && steps != 0;
  }
  public void goBackOrForward(int steps) {
    if (!canGoBackOrForward(steps)) return;
    index += steps;
    Entry e = history.get(index);
    start(e.url, isLocalDocument(e.url) || URLUtil.isDataUrl(e.url), false);
  }
  public void clearHistory() {
    if (index < 0) return;
    Entry current = history.get(index);
    history.clear();
    history.add(current);
    index = 0;
  }
  public WebBackForwardList copyBackForwardList() {
    List<Entry> items = List.copyOf(history);
    int current = index;
    return new Snapshot(items, current);
  }
  public String getUrl() { return index < 0 ? null : history.get(index).url; }
  public String getOriginalUrl() { return index < 0 ? null : history.get(index).originalUrl; }
  public String getTitle() { return index < 0 ? null : history.get(index).title; }
  public Bitmap getFavicon() { return null; }
  public int getProgress() { return progress; }
  public int getContentHeight() { return 0; }

  private static boolean isLocalDocument(String url) { return URLUtil.isAboutUrl(url) || URLUtil.isDataUrl(url); }

  private void navigate(String url, String historyUrl, boolean loads) {
    while (history.size() > index + 1) history.remove(history.size() - 1);
    String shown = historyUrl != null ? historyUrl : url;
    history.add(new Entry(shown, url, shown));
    index = history.size() - 1;
    start(url, loads, false);
  }
  private void start(String url, boolean loads, boolean reload) {
    if (!loads && !rendererUnavailableReported) {
      rendererUnavailableReported = true;
      android.util.Log.w("WebView", "No web engine: network and file pages report a load error");
    }
    int id = ++navigation;
    progress = 10;
    String shown = getUrl();
    post(() -> {
      if (destroyed || id != navigation) return;
      client.onPageStarted(this, shown, null);
      if (chromeClient != null) chromeClient.onProgressChanged(this, progress);
      if (loads) {
        client.onPageCommitVisible(this, shown);
        client.doUpdateVisitedHistory(this, shown, reload);
      } else {
        client.onReceivedError(this, new Request(url, true), new LoadError(WebViewClient.ERROR_UNKNOWN,
            "net::ERR_NOT_IMPLEMENTED"));
      }
      if (destroyed || id != navigation) return;
      progress = 100;
      if (chromeClient != null) {
        chromeClient.onReceivedTitle(this, getTitle());
        chromeClient.onProgressChanged(this, progress);
      }
      client.onPageFinished(this, shown);
    });
  }

  // Scripts and bridges.
  public void evaluateJavascript(String script, ValueCallback<String> resultCallback) {
    if (destroyed || resultCallback == null) return;
    // Chromium reports an evaluation with no result as the JSON string "null".
    post(() -> resultCallback.onReceiveValue("null"));
  }
  public void addJavascriptInterface(Object object, String name) {
    if (destroyed || object == null || name == null) return;
    synchronized (javascriptInterfaces) { javascriptInterfaces.put(name, object); }
  }
  public void removeJavascriptInterface(String name) {
    synchronized (javascriptInterfaces) { javascriptInterfaces.remove(name); }
  }
  public void postVisualStateCallback(long requestId, VisualStateCallback callback) {
    if (!destroyed && callback != null) post(() -> callback.onComplete(requestId));
  }

  // Find in page: there is no document text, so every search completes with no matches.
  public void findAllAsync(String find) {
    if (destroyed) return;
    findQuery = find;
    reportFind();
  }
  @Deprecated public int findAll(String find) {
    findAllAsync(find);
    return 0;
  }
  public void findNext(boolean forward) { if (!destroyed && findQuery != null) reportFind(); }
  public void clearMatches() { findQuery = null; }
  private void reportFind() {
    FindListener listener = findListener;
    if (listener != null) post(() -> listener.onFindResultReceived(0, 0, true));
  }

  // Lifecycle and state.
  public void onPause() { paused = true; }
  public void onResume() { paused = false; }
  public void pauseTimers() {}
  public void resumeTimers() {}
  public void destroy() {
    if (destroyed) return;
    destroyed = true;
    ++navigation;
    synchronized (javascriptInterfaces) { javascriptInterfaces.clear(); }
    findListener = null;
    downloadListener = null;
  }
  public void clearCache(boolean includeDiskFiles) {}
  public void clearFormData() {}
  public void clearSslPreferences() {}
  public void freeMemory() {}
  public void setNetworkAvailable(boolean networkUp) { networkAvailable = networkUp; }
  public void setRendererPriorityPolicy(int priority, boolean waivedWhenNotVisible) {
    rendererPriority = priority;
    rendererWaivedWhenNotVisible = waivedWhenNotVisible;
  }
  public int getRendererRequestedPriority() { return rendererPriority; }
  public boolean getRendererPriorityWaivedWhenNotVisible() { return rendererWaivedWhenNotVisible; }
  public void requestFocusNodeHref(Message hrefMsg) {
    if (hrefMsg == null) return;
    var data = hrefMsg.getData();
    data.putString("url", null);
    data.putString("title", null);
    data.putString("src", null);
    hrefMsg.setData(data);
    hrefMsg.sendToTarget();
  }
  public void requestImageRef(Message msg) {
    if (msg == null) return;
    var data = msg.getData();
    data.putString("url", null);
    msg.setData(data);
    msg.sendToTarget();
  }
  public HitTestResult getHitTestResult() { return new HitTestResult(); }
  public android.print.PrintDocumentAdapter createPrintDocumentAdapter(String documentName) {
    return new android.print.PrintDocumentAdapter() {};
  }
  @Deprecated public android.print.PrintDocumentAdapter createPrintDocumentAdapter() {
    return createPrintDocumentAdapter("default");
  }

  // Zoom and scrolling.
  public void setInitialScale(int scaleInPercent) { initialScale = scaleInPercent; }
  @Deprecated public float getScale() { return scale; }
  /** AOSP: factor is clamped by the page; values outside [0.01, 100] are rejected. */
  public void zoomBy(float zoomFactor) {
    if (zoomFactor < 0.01f) throw new IllegalArgumentException("zoomFactor must be greater than 0.01.");
    if (zoomFactor > 100f) throw new IllegalArgumentException("zoomFactor must be less than 100.");
  }
  public boolean zoomIn() { return false; }
  public boolean zoomOut() { return false; }
  public boolean pageUp(boolean top) { return false; }
  public boolean pageDown(boolean bottom) { return false; }
  public void flingScroll(int vx, int vy) {}
  public boolean overlayHorizontalScrollbar() { return true; }
  public boolean overlayVerticalScrollbar() { return false; }

  // Snapshot types handed to the app.
  private static final class Item extends WebHistoryItem {
    private final Entry entry;
    Item(Entry entry) { this.entry = entry; }
    @Override public String getUrl() { return entry.url; }
    @Override public String getOriginalUrl() { return entry.originalUrl; }
    @Override public String getTitle() { return entry.title; }
    @Override public Bitmap getFavicon() { return null; }
    @Override protected Item clone() { return new Item(entry); }
  }
  private static final class Snapshot extends WebBackForwardList {
    private final List<Entry> items;
    private final int current;
    Snapshot(List<Entry> items, int current) { this.items = items; this.current = current; }
    @Override public WebHistoryItem getCurrentItem() { return current < 0 ? null : new Item(items.get(current)); }
    @Override public int getCurrentIndex() { return current; }
    @Override public WebHistoryItem getItemAtIndex(int i) {
      return i < 0 || i >= items.size() ? null : new Item(items.get(i));
    }
    @Override public int getSize() { return items.size(); }
    @Override protected Snapshot clone() { return new Snapshot(items, current); }
  }
  private static final class Request implements WebResourceRequest {
    private final Uri url;
    private final boolean mainFrame;
    Request(String url, boolean mainFrame) { this.url = Uri.parse(url); this.mainFrame = mainFrame; }
    @Override public Uri getUrl() { return url; }
    @Override public boolean isForMainFrame() { return mainFrame; }
    @Override public boolean hasGesture() { return false; }
    @Override public String getMethod() { return "GET"; }
    @Override public Map<String, String> getRequestHeaders() { return Map.of(); }
  }
  private static final class LoadError extends WebResourceError {
    private final int code;
    private final String description;
    LoadError(int code, String description) { this.code = code; this.description = description; }
    @Override public int getErrorCode() { return code; }
    @Override public CharSequence getDescription() { return description; }
  }
}
