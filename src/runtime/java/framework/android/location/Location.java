package android.location;

/** Value object; coordinates are supplied by callers, never synthesized sensors. */
public class Location {
  private String provider;
  private double latitude, longitude, altitude;
  private float accuracy, speed, bearing;
  private long time, elapsed;
  private boolean hasAltitude, hasAccuracy, hasSpeed, hasBearing;
  public Location(String provider) { this.provider = provider; }
  public String getProvider() { return provider; }
  public void setProvider(String v) { provider = v; }
  public double getLatitude() { return latitude; }
  public void setLatitude(double v) { latitude = v; }
  public double getLongitude() { return longitude; }
  public void setLongitude(double v) { longitude = v; }
  public double getAltitude() { return altitude; }
  public void setAltitude(double v) { altitude = v; hasAltitude = true; }
  public boolean hasAltitude() { return hasAltitude; }
  public void removeAltitude() { altitude = 0; hasAltitude = false; }
  public float getAccuracy() { return accuracy; }
  public void setAccuracy(float v) { accuracy = v; hasAccuracy = true; }
  public boolean hasAccuracy() { return hasAccuracy; }
  public void removeAccuracy() { accuracy = 0; hasAccuracy = false; }
  public float getSpeed() { return speed; }
  public void setSpeed(float v) { speed = v; hasSpeed = true; }
  public boolean hasSpeed() { return hasSpeed; }
  public void removeSpeed() { speed = 0; hasSpeed = false; }
  public float getBearing() { return bearing; }
  public void setBearing(float v) { bearing = (v % 360 + 360) % 360; hasBearing = true; }
  public boolean hasBearing() { return hasBearing; }
  public void removeBearing() { bearing = 0; hasBearing = false; }
  public long getTime() { return time; }
  public void setTime(long v) { time = v; }
  public long getElapsedRealtimeNanos() { return elapsed; }
  public void setElapsedRealtimeNanos(long v) { elapsed = v; }
}
