package android.webkit;

import java.io.File;
import java.io.IOException;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.time.ZonedDateTime;
import java.time.format.DateTimeFormatter;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * AOSP android.webkit.CookieManager: the app-wide WebView cookie store, also used
 * by HTTP stacks (okhttp cookie jars) that share cookies with WebView. Matching
 * follows RFC 6265 like Chromium's store: host-only and Domain cookies, default
 * and explicit paths, Secure, Expires/Max-Age, and the __Secure-/__Host-
 * prefixes. Persistent cookies survive restarts in the app's
 * app_webview/Default directory, where Android WebView keeps its Cookies
 * database; session cookies live only in memory. Writes are flushed shortly
 * after each change and synchronously by flush().
 */
public class CookieManager {
  private record Key(String name, String domain, String path) {}
  private static final class Cookie {
    final String name, value, domain, path;
    final boolean hostOnly, secure, httpOnly;
    final long expiry; // ms since epoch; Long.MAX_VALUE for session cookies
    final long creation;
    Cookie(String name, String value, String domain, String path, boolean hostOnly, boolean secure,
        boolean httpOnly, long expiry, long creation) {
      this.name = name; this.value = value; this.domain = domain; this.path = path;
      this.hostOnly = hostOnly; this.secure = secure; this.httpOnly = httpOnly;
      this.expiry = expiry; this.creation = creation;
    }
    boolean persistent() { return expiry != Long.MAX_VALUE; }
  }

  private static CookieManager instance;
  private static boolean acceptFileScheme;
  private final Map<Key, Cookie> cookies = new LinkedHashMap<>();
  private final File file;
  private boolean accept = true, loaded, savePending;
  private long lastCreation;

  /** AOSP: the singleton is created on first use, after the app's data directory exists. */
  public static synchronized CookieManager getInstance() {
    if (instance == null)
      instance = new CookieManager(
          new File(com.venture.runtime.RuntimeEnvironment.get().data, "app_webview/Default/Cookies.venture"));
    return instance;
  }
  /** Store backed by {@code file}, or memory only when it is null. */
  CookieManager(File file) { this.file = file; }
  @Deprecated public CookieManager() { this(null); }

  public synchronized void setAcceptCookie(boolean accept) { this.accept = accept; }
  public synchronized boolean acceptCookie() { return accept; }
  public void setAcceptThirdPartyCookies(WebView webview, boolean accept) {
    if (webview != null) webview.acceptThirdPartyCookies = accept;
  }
  public boolean acceptThirdPartyCookies(WebView webview) {
    return webview != null && webview.acceptThirdPartyCookies;
  }
  public static boolean allowFileSchemeCookies() { return acceptFileScheme; }
  @Deprecated public static void setAcceptFileSchemeCookies(boolean accept) { acceptFileScheme = accept; }

  public void setCookie(String url, String value) { set(url, value); }
  public void setCookie(String url, String value, ValueCallback<Boolean> callback) {
    boolean result = set(url, value);
    deliver(callback, result);
  }
  public String getCookie(String url) {
    URI uri = parse(url);
    if (uri == null) return null;
    String host = uri.getHost().toLowerCase(Locale.ROOT);
    String path = uri.getRawPath() == null || uri.getRawPath().isEmpty() ? "/" : uri.getRawPath();
    boolean secure = "https".equalsIgnoreCase(uri.getScheme()) || "wss".equalsIgnoreCase(uri.getScheme());
    long now = System.currentTimeMillis();
    List<Cookie> matches = new ArrayList<>();
    synchronized (this) {
      load();
      boolean expired = cookies.values().removeIf(c -> c.expiry <= now);
      if (expired) scheduleSave();
      for (Cookie c : cookies.values())
        if ((c.hostOnly ? host.equals(c.domain) : domainMatches(host, c.domain))
            && pathMatches(path, c.path) && (!c.secure || secure))
          matches.add(c);
    }
    if (matches.isEmpty()) return null;
    // RFC 6265 5.4: longer paths first, then earlier creation.
    matches.sort((a, b) -> a.path.length() != b.path.length() ? b.path.length() - a.path.length()
        : Long.compare(a.creation, b.creation));
    StringBuilder out = new StringBuilder();
    for (Cookie c : matches) {
      if (out.length() > 0) out.append("; ");
      out.append(c.name.isEmpty() ? c.value : c.name + "=" + c.value);
    }
    return out.toString();
  }
  public String getCookie(String url, boolean privateBrowsing) { return getCookie(url); }

  public void removeSessionCookies(ValueCallback<Boolean> callback) {
    boolean removed;
    synchronized (this) {
      load();
      removed = cookies.values().removeIf(c -> !c.persistent());
    }
    deliver(callback, removed);
  }
  public void removeAllCookies(ValueCallback<Boolean> callback) {
    boolean removed;
    synchronized (this) {
      load();
      removed = !cookies.isEmpty();
      cookies.clear();
      if (removed) scheduleSave();
    }
    deliver(callback, removed);
  }
  @Deprecated public void removeSessionCookie() { removeSessionCookies(null); }
  @Deprecated public void removeAllCookie() { removeAllCookies(null); }
  @Deprecated public synchronized void removeExpiredCookie() {
    load();
    long now = System.currentTimeMillis();
    if (cookies.values().removeIf(c -> c.expiry <= now)) scheduleSave();
  }
  public synchronized boolean hasCookies() {
    load();
    long now = System.currentTimeMillis();
    for (Cookie c : cookies.values()) if (c.expiry > now) return true;
    return false;
  }
  @Deprecated public boolean hasCookies(boolean privateBrowsing) { return hasCookies(); }
  public synchronized void flush() {
    savePending = false;
    save();
  }

  // Chromium's adapter accepts scheme-less URLs as http (WebAddress fixup).
  private static URI parse(String url) {
    if (url == null || url.isEmpty()) return null;
    String fixed = url.contains("://") ? url : "http://" + url;
    try {
      URI uri = new URI(fixed);
      return uri.getHost() == null ? null : uri;
    } catch (Exception e) {
      return null;
    }
  }
  static boolean domainMatches(String host, String domain) {
    return host.equals(domain) || (host.endsWith("." + domain) && !isIpAddress(host));
  }
  private static boolean isIpAddress(String host) {
    return host.indexOf(':') >= 0 || host.matches("[0-9.]+");
  }
  static boolean pathMatches(String requestPath, String cookiePath) {
    if (requestPath.equals(cookiePath)) return true;
    if (!requestPath.startsWith(cookiePath)) return false;
    return cookiePath.endsWith("/") || requestPath.charAt(cookiePath.length()) == '/';
  }
  // RFC 6265 5.1.4 default-path: the request path up to, not including, its last '/'.
  private static String defaultPath(String path) {
    if (path == null || path.isEmpty() || path.charAt(0) != '/') return "/";
    int last = path.lastIndexOf('/');
    return last == 0 ? "/" : path.substring(0, last);
  }

  private boolean set(String url, String header) {
    URI uri = parse(url);
    if (uri == null || header == null) return false;
    if ("file".equalsIgnoreCase(uri.getScheme()) && !acceptFileScheme) return false;
    String host = uri.getHost().toLowerCase(Locale.ROOT);
    boolean secureOrigin = "https".equalsIgnoreCase(uri.getScheme()) || "wss".equalsIgnoreCase(uri.getScheme());
    String[] parts = header.split(";");
    String pair = parts[0];
    int eq = pair.indexOf('=');
    String name = eq < 0 ? "" : pair.substring(0, eq).trim();
    String value = (eq < 0 ? pair : pair.substring(eq + 1)).trim();
    if (name.isEmpty() && value.isEmpty()) return false;
    String domain = null, path = null;
    boolean secure = false, httpOnly = false;
    long expiry = Long.MAX_VALUE, now = System.currentTimeMillis();
    boolean maxAgeSeen = false;
    for (int i = 1; i < parts.length; i++) {
      String attr = parts[i].trim();
      int aeq = attr.indexOf('=');
      String key = (aeq < 0 ? attr : attr.substring(0, aeq)).trim().toLowerCase(Locale.ROOT);
      String av = aeq < 0 ? "" : attr.substring(aeq + 1).trim();
      switch (key) {
        case "domain" -> {
          String d = av.startsWith(".") ? av.substring(1) : av;
          if (!d.isEmpty()) domain = d.toLowerCase(Locale.ROOT);
        }
        case "path" -> path = av.startsWith("/") ? av : null;
        case "secure" -> secure = true;
        case "httponly" -> httpOnly = true;
        case "max-age" -> {
          try {
            long seconds = Long.parseLong(av);
            expiry = seconds <= 0 ? Long.MIN_VALUE : now + Math.min(seconds, Long.MAX_VALUE / 2000) * 1000;
            maxAgeSeen = true;
          } catch (NumberFormatException ignored) {}
        }
        case "expires" -> {
          if (!maxAgeSeen) {
            long parsed = parseDate(av);
            if (parsed != Long.MIN_VALUE) expiry = parsed;
          }
        }
        default -> {}
      }
    }
    if (secure && !secureOrigin) return false;
    boolean hostOnly = domain == null;
    if (hostOnly) {
      domain = host;
    } else if (!domainMatches(host, domain) || (domain.indexOf('.') < 0 && !domain.equals(host))) {
      return false;
    }
    if (path == null) path = defaultPath(uri.getRawPath());
    if (name.startsWith("__Secure-") && !secure) return false;
    if (name.startsWith("__Host-") && (!secure || !hostOnly || !"/".equals(path))) return false;
    Key key = new Key(name, domain, path);
    synchronized (this) {
      load();
      Cookie old = cookies.remove(key);
      if (expiry <= now) {
        if (old != null && old.persistent()) scheduleSave();
        return true;
      }
      long creation = old != null ? old.creation : nextCreation(now);
      Cookie cookie = new Cookie(name, value, domain, path, hostOnly, secure, httpOnly, expiry, creation);
      cookies.put(key, cookie);
      if (cookie.persistent() || (old != null && old.persistent())) scheduleSave();
    }
    return true;
  }
  // Cookie dates (RFC 6265 5.1.1 is lenient): the weekday is ignored, '-' may
  // separate the date, and two-digit years are 1970-2069.
  private static final DateTimeFormatter[] DATES = {
      DateTimeFormatter.ofPattern("d MMM yyyy HH:mm:ss zzz", Locale.ROOT),
      DateTimeFormatter.ofPattern("d MMM yy HH:mm:ss zzz", Locale.ROOT)};
  private static long parseDate(String value) {
    String s = value.replaceFirst("^[A-Za-z]+,?\\s*", "").replace('-', ' ').replaceAll("\\s+", " ").trim();
    for (int i = 0; i < DATES.length; i++) {
      try {
        ZonedDateTime date = ZonedDateTime.parse(s, DATES[i]);
        if (i == 1 && date.getYear() >= 2070) date = date.minusYears(100);
        return date.toInstant().toEpochMilli();
      } catch (RuntimeException ignored) {}
    }
    return Long.MIN_VALUE;
  }
  private long nextCreation(long now) {
    lastCreation = Math.max(now, lastCreation + 1);
    return lastCreation;
  }

  private static void deliver(ValueCallback<Boolean> callback, boolean value) {
    if (callback == null) return;
    // AOSP: the callback runs on the calling thread's Looper.
    android.os.Looper looper = android.os.Looper.myLooper();
    if (looper == null) throw new IllegalStateException("CookieManager callbacks need a thread with a Looper");
    new android.os.Handler(looper).post(() -> callback.onReceiveValue(value));
  }

  // Persistence: one tab-separated line per persistent cookie.
  private void load() {
    if (loaded) return;
    loaded = true;
    if (file == null || !file.isFile()) return;
    try {
      for (String line : Files.readAllLines(file.toPath(), StandardCharsets.UTF_8)) {
        String[] f = line.split("\t", -1);
        if (f.length != 9) continue;
        Cookie c = new Cookie(f[0], f[1], f[2], f[3], f[4].equals("1"), f[5].equals("1"), f[6].equals("1"),
            Long.parseLong(f[7]), Long.parseLong(f[8]));
        cookies.put(new Key(c.name, c.domain, c.path), c);
        lastCreation = Math.max(lastCreation, c.creation);
      }
    } catch (IOException | RuntimeException e) {
      android.util.Log.w("CookieManager", "Cookie store unreadable; starting empty: " + e.getClass().getSimpleName());
      cookies.clear();
    }
  }
  private void scheduleSave() {
    if (file == null || savePending) return;
    savePending = true;
    saver.schedule(() -> {
      synchronized (this) {
        if (!savePending) return;
        savePending = false;
        save();
      }
    }, 1, java.util.concurrent.TimeUnit.SECONDS);
  }
  private void save() {
    if (file == null || !loaded) return;
    long now = System.currentTimeMillis();
    StringBuilder out = new StringBuilder();
    for (Cookie c : cookies.values()) {
      if (!c.persistent() || c.expiry <= now) continue;
      if ((c.name + c.value).indexOf('\t') >= 0 || (c.name + c.value).indexOf('\n') >= 0) continue;
      out.append(c.name).append('\t').append(c.value).append('\t').append(c.domain).append('\t')
          .append(c.path).append('\t').append(c.hostOnly ? 1 : 0).append('\t').append(c.secure ? 1 : 0)
          .append('\t').append(c.httpOnly ? 1 : 0).append('\t').append(c.expiry).append('\t')
          .append(c.creation).append('\n');
    }
    try {
      File dir = file.getParentFile();
      if (dir != null) dir.mkdirs();
      File tmp = new File(dir, file.getName() + ".tmp");
      Files.writeString(tmp.toPath(), out, StandardCharsets.UTF_8);
      Files.move(tmp.toPath(), file.toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
    } catch (IOException e) {
      android.util.Log.w("CookieManager", "Cookie store not written: " + e.getClass().getSimpleName());
    }
  }
  private static final java.util.concurrent.ScheduledExecutorService saver =
      java.util.concurrent.Executors.newSingleThreadScheduledExecutor(r -> {
        Thread t = new Thread(r, "CookieManager-flush");
        t.setDaemon(true);
        return t;
      });
}
