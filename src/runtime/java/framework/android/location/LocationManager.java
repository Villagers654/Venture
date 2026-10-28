package android.location;
import java.util.List;

/** The runtime currently exposes no host geographic-location providers. */
public final class LocationManager {
  public static final String GPS_PROVIDER = "gps", NETWORK_PROVIDER = "network", PASSIVE_PROVIDER = "passive";
  public List<String> getAllProviders() { return List.of(); }
  public List<String> getProviders(boolean enabledOnly) { return List.of(); }
  public boolean isProviderEnabled(String provider) {
    java.util.Objects.requireNonNull(provider);
    return false;
  }
  public Location getLastKnownLocation(String provider) {
    java.util.Objects.requireNonNull(provider);
    android.util.Log.w("LocationManager", "No geographic location provider: " + provider);
    return null;
  }
  public void requestLocationUpdates(String provider, long minTime, float minDistance, LocationListener listener) {
    throw new IllegalArgumentException("Geographic location provider unavailable: " + provider);
  }
  public void removeUpdates(LocationListener listener) {
    java.util.Objects.requireNonNull(listener);
    // No registrations can exist because requestLocationUpdates rejects absent providers.
  }
}
