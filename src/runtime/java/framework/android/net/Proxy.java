package android.net;

import android.content.Context;

/** AOSP android.net.Proxy: the global HTTP proxy, read from the http.proxy* system properties. */
public final class Proxy {
  public static final String PROXY_CHANGE_ACTION = "android.intent.action.PROXY_CHANGE";
  private Proxy() {}
  @Deprecated public static String getHost(Context ctx) { return getDefaultHost(); }
  @Deprecated public static int getPort(Context ctx) { return getDefaultPort(); }
  @Deprecated public static String getDefaultHost() {
    String host = System.getProperty("http.proxyHost");
    return host == null || host.isEmpty() ? null : host;
  }
  @Deprecated public static int getDefaultPort() {
    if (getDefaultHost() == null) return -1;
    try {
      return Integer.parseInt(System.getProperty("http.proxyPort"));
    } catch (NumberFormatException e) {
      return -1;
    }
  }
}
