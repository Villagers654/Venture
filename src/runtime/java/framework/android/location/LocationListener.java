package android.location;
public interface LocationListener {
  void onLocationChanged(Location location);
  default void onStatusChanged(String provider, int status, android.os.Bundle extras) {}
  default void onProviderEnabled(String provider) {}
  default void onProviderDisabled(String provider) {}
}
