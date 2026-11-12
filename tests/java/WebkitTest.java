import android.media.MediaDrm;
import android.media.UnsupportedSchemeException;
import android.os.Handler;
import android.os.Looper;
import android.webkit.*;
import java.io.File;
import java.nio.file.Files;
import java.util.*;
import java.util.concurrent.*;

/** android.webkit without a web engine, CookieManager matching/persistence, and DRM-less MediaDrm. */
public final class WebkitTest {
  private static int checks;
  private static Handler main;
  private static void check(boolean ok, String message) {
    checks++;
    if (!ok) throw new AssertionError(message);
  }
  private static <T> T onMain(Callable<T> work) throws Exception {
    var task = new FutureTask<>(work);
    main.post(task);
    return task.get(5, TimeUnit.SECONDS);
  }
  private static void drain() throws Exception { onMain(() -> null); }

  private static final List<String> logs = new CopyOnWriteArrayList<>();
  public static void main(String[] args) throws Exception {
    // Log.* reaches the native host; record it instead.
    var hostField = com.venture.runtime.RuntimeEnvironment.class.getDeclaredField("host");
    hostField.setAccessible(true);
    hostField.set(null, java.lang.reflect.Proxy.newProxyInstance(com.venture.runtime.Host.class.getClassLoader(),
        new Class<?>[] {com.venture.runtime.Host.class}, (proxy, method, a) -> {
          if (!method.getName().equals("log")) throw new UnsupportedOperationException(method.getName());
          logs.add(a[1] + ": " + a[2]);
          return null;
        }));
    var ready = new CountDownLatch(1);
    var looper = new Thread(() -> {
      Looper.prepareMainLooper();
      main = new Handler(Looper.myLooper());
      ready.countDown();
      Looper.loop();
    }, "main");
    looper.setDaemon(true);
    looper.start();
    ready.await();
    webView();
    cookies();
    mediaDrm();
    helpers();
    check(!new android.media.AudioManager().isVolumeFixed(), "Quest media volume is adjustable");
    System.out.println("WebkitTest: " + checks + " checks passed");
  }

  private static void webView() throws Exception {
    var events = new CopyOnWriteArrayList<String>();
    var context = new android.content.Context();
    WebView view = onMain(() -> new WebView(context));
    check(view instanceof android.widget.AbsoluteLayout && view.getSettings() != null, "WebView is an AbsoluteLayout with settings");
    check(view.getUrl() == null && !view.canGoBack() && view.getProgress() == 100, "a new WebView has no page");
    var client = new WebViewClient() {
      @Override public void onPageStarted(WebView v, String url, android.graphics.Bitmap icon) { events.add("start " + url); }
      @Override public void onPageFinished(WebView v, String url) { events.add("finish " + url); }
      @Override public void onReceivedError(WebView v, int code, String description, String url) {
        events.add("error " + code + " " + url);
      }
    };
    var chrome = new WebChromeClient() {
      @Override public void onProgressChanged(WebView v, int progress) { events.add("progress " + progress); }
    };
    onMain(() -> {
      view.setWebViewClient(client);
      view.setWebChromeClient(chrome);
      view.getSettings().setJavaScriptEnabled(true);
      view.getSettings().setUserAgentString("custom");
      view.loadUrl("about:blank");
      return null;
    });
    drain();
    check(events.equals(List.of("start about:blank", "progress 10", "progress 100", "finish about:blank")),
        "about:blank loads with started, progress and finished callbacks: " + events);
    check(view.getSettings().getJavaScriptEnabled() && view.getSettings().getUserAgentString().equals("custom"),
        "settings read back");
    onMain(() -> { view.getSettings().setUserAgentString(null); return null; });
    check(view.getSettings().getUserAgentString().contains("; wv)"), "null user agent restores the WebView default");

    events.clear();
    onMain(() -> { view.loadUrl("https://example.com/page"); return null; });
    drain();
    check(events.equals(List.of("start https://example.com/page", "progress 10",
        "error " + WebViewClient.ERROR_UNKNOWN + " https://example.com/page", "progress 100",
        "finish https://example.com/page")), "a network page reports a main-frame error, then finishes: " + events);
    check(logs.stream().filter(l -> l.startsWith("WebView: No web engine")).count() == 1, "the missing engine is logged once");
    check(view.canGoBack() && !view.canGoForward() && view.getUrl().equals("https://example.com/page"), "history records loads");
    var list = view.copyBackForwardList();
    check(list.getSize() == 2 && list.getCurrentIndex() == 1 && list.getItemAtIndex(0).getUrl().equals("about:blank"),
        "back/forward list snapshot");
    onMain(() -> { view.goBack(); return null; });
    drain();
    check(view.getUrl().equals("about:blank") && view.canGoForward(), "goBack moves within history");
    onMain(() -> { view.loadData("<p>x</p>", "text/html", null); return null; });
    check(!view.canGoForward(), "a new load drops forward history");

    var values = new CopyOnWriteArrayList<Object>();
    onMain(() -> {
      view.evaluateJavascript("1+1", values::add);
      view.setFindListener((active, count, done) -> values.add(active + "/" + count + "/" + done));
      view.findAllAsync("text");
      view.addJavascriptInterface(new Object(), "bridge");
      return null;
    });
    drain();
    check(values.equals(List.of("null", "0/0/true")), "script and find callbacks run on the UI thread: " + values);
    boolean threw = false;
    try { view.zoomBy(0f); } catch (IllegalArgumentException expected) { threw = true; }
    check(threw, "zoomBy rejects factors below 0.01");

    events.clear();
    onMain(() -> { view.loadUrl("about:blank"); view.destroy(); view.loadUrl("about:blank"); return null; });
    drain();
    check(events.isEmpty(), "destroy cancels pending and later loads: " + events);
  }

  private static CookieManager store(File file) throws Exception {
    var ctor = CookieManager.class.getDeclaredConstructor(File.class);
    ctor.setAccessible(true);
    return ctor.newInstance(file);
  }

  private static void cookies() throws Exception {
    File dir = Files.createTempDirectory("venture-cookies").toFile();
    File file = new File(dir, "app_webview/Default/Cookies.venture");
    CookieManager cookies = store(file);
    check(!cookies.hasCookies() && cookies.getCookie("https://example.com/") == null, "an empty store has no cookies");
    cookies.setCookie("https://example.com/a/b", "session=1");
    cookies.setCookie("https://example.com/", "persist=2; Max-Age=3600; Path=/");
    cookies.setCookie("https://example.com/", "wide=3; Domain=.example.com; Path=/; Max-Age=3600");
    cookies.setCookie("https://example.com/", "deep=4; Path=/a/b/c");
    cookies.setCookie("https://example.com/", "sec=5; Secure; Path=/");
    cookies.setCookie("http://example.com/", "insecure=6; Secure");
    cookies.setCookie("https://example.com/", "other=7; Domain=other.com");
    check(cookies.getCookie("https://example.com/a/b/c").equals("deep=4; session=1; persist=2; wide=3; sec=5"),
        "path, host-only and secure matching, longest path first: " + cookies.getCookie("https://example.com/a/b/c"));
    check(cookies.getCookie("http://example.com/").equals("persist=2; wide=3"), "secure cookies stay off http");
    check(cookies.getCookie("https://www.example.com/").equals("wide=3"),
        "only Domain cookies reach subdomains: " + cookies.getCookie("https://www.example.com/"));
    check(cookies.getCookie("https://example.com/ab") .equals("persist=2; wide=3; sec=5"), "/a does not path-match /ab");
    check(cookies.getCookie("example.com/a/b").equals("session=1; persist=2; wide=3"), "scheme-less URLs are http");
    cookies.setCookie("https://example.com/", "persist=gone; Max-Age=0; Path=/");
    check(!cookies.getCookie("https://example.com/").contains("persist"), "Max-Age=0 deletes a cookie");
    cookies.setCookie("https://example.com/", "persist=2; Expires=Wed, 01 Jan 2200 00:00:00 GMT; Path=/");
    cookies.setCookie("https://example.com/", "__Host-bad=1; Path=/x; Secure");
    check(!cookies.getCookie("https://example.com/x").contains("__Host-bad"), "__Host- needs Path=/");
    cookies.flush();
    CookieManager reloaded = store(file);
    check(reloaded.getCookie("https://example.com/").equals("wide=3; persist=2"),
        "persistent cookies survive a restart, session cookies do not: " + reloaded.getCookie("https://example.com/"));

    var results = new CopyOnWriteArrayList<Boolean>();
    onMain(() -> {
      cookies.removeSessionCookies(results::add);
      cookies.removeAllCookies(results::add);
      cookies.removeAllCookies(results::add);
      return null;
    });
    drain();
    check(results.equals(List.of(true, true, false)), "remove callbacks report whether cookies were removed: " + results);
    check(!cookies.hasCookies(), "removeAllCookies empties the store");
    cookies.flush();
    check(!store(file).hasCookies(), "removal is persisted");
    boolean threw = false;
    try { cookies.removeAllCookies(v -> {}); } catch (IllegalStateException expected) { threw = true; }
    check(threw, "async callbacks need a Looper thread, as on Android");
  }

  private static void mediaDrm() {
    var widevine = UUID.fromString("edef8ba9-79d6-4ace-a3c8-27dcd51d21ed");
    check(!MediaDrm.isCryptoSchemeSupported(widevine) && !MediaDrm.isCryptoSchemeSupported(widevine, "video/mp4")
        && MediaDrm.getSupportedCryptoSchemes().isEmpty(), "no DRM scheme is supported");
    boolean unsupported = false;
    try { new MediaDrm(widevine); } catch (UnsupportedSchemeException expected) { unsupported = true; }
    check(unsupported, "constructing a MediaDrm throws UnsupportedSchemeException");
    MediaDrm.OnExpirationUpdateListener expiration = (md, session, time) -> {};
    MediaDrm.OnKeyStatusChangeListener status = (md, session, keys, usable) -> {};
    MediaDrm.OnEventListener event = (md, session, e, extra, data) -> {};
    check(expiration != null && status != null && event != null, "player listener types exist");
  }

  private static void helpers() {
    var mime = MimeTypeMap.getSingleton();
    check("video/mp4".equals(mime.getMimeTypeFromExtension("MP4")) && "jpg".equals(mime.getExtensionFromMimeType("image/jpeg")),
        "MIME table lookups");
    check(MimeTypeMap.getFileExtensionFromUrl("https://x.y/a/clip.mkv?t=1#f").equals("mkv"), "extension from URL");
    check(URLUtil.guessFileName("https://x.y/files/movie.mp4?dl=1", null, null).equals("movie.mp4"), "file name from URL");
    check(URLUtil.guessFileName("https://x.y/get", "attachment; filename=\"clip.webm\"", "video/webm").equals("clip.webm"),
        "file name from Content-Disposition");
    check(URLUtil.guessFileName("https://x.y/", null, "video/mp4").equals("downloadfile.mp4"), "fallback file name");
    check(URLUtil.isNetworkUrl("https://a") && !URLUtil.isNetworkUrl("file:///a") && URLUtil.isDataUrl("data:,x"), "URL classes");
  }
}
