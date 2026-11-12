package android.webkit;

import java.util.HashMap;
import java.util.Map;
import java.util.Set;

/** AOSP android.webkit.GeolocationPermissions: per-origin geolocation grants of WebViews. */
public class GeolocationPermissions {
  public interface Callback {
    void invoke(String origin, boolean allow, boolean retain);
  }
  private static final GeolocationPermissions instance = new GeolocationPermissions();
  private final Map<String, Boolean> origins = new HashMap<>();
  protected GeolocationPermissions() {}
  public static GeolocationPermissions getInstance() { return instance; }
  public synchronized void getOrigins(ValueCallback<Set<String>> callback) {
    if (callback != null) callback.onReceiveValue(Set.copyOf(origins.keySet()));
  }
  public synchronized void getAllowed(String origin, ValueCallback<Boolean> callback) {
    if (callback != null) callback.onReceiveValue(origin != null && origins.getOrDefault(origin, false));
  }
  public synchronized void allow(String origin) { if (origin != null) origins.put(origin, true); }
  public synchronized void clear(String origin) { origins.remove(origin); }
  public synchronized void clearAll() { origins.clear(); }
}
