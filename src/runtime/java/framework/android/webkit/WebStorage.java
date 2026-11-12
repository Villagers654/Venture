package android.webkit;

import java.util.Map;

/**
 * AOSP android.webkit.WebStorage: per-origin web storage of WebViews. Venture's
 * WebView keeps no page storage, so there are no origins and nothing to delete.
 */
public class WebStorage {
  public interface QuotaUpdater {
    void updateQuota(long newQuota);
  }
  public static class Origin {
    private final String origin;
    private final long quota, usage;
    protected Origin(String origin, long quota, long usage) {
      this.origin = origin; this.quota = quota; this.usage = usage;
    }
    public String getOrigin() { return origin; }
    public long getQuota() { return quota; }
    public long getUsage() { return usage; }
  }
  private static final WebStorage instance = new WebStorage();
  public WebStorage() {}
  public static WebStorage getInstance() { return instance; }
  public void getOrigins(ValueCallback<Map> callback) {
    if (callback != null) callback.onReceiveValue(new java.util.HashMap<String, Origin>());
  }
  public void getUsageForOrigin(String origin, ValueCallback<Long> callback) {
    if (callback != null) callback.onReceiveValue(0L);
  }
  public void getQuotaForOrigin(String origin, ValueCallback<Long> callback) {
    if (callback != null) callback.onReceiveValue(0L);
  }
  public void deleteOrigin(String origin) {}
  public void deleteAllData() {}
}
