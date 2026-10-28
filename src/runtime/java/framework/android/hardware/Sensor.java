package android.hardware;
public final class Sensor {
  // AOSP core/java/android/hardware/Sensor.java; a type identifier is not a
  // claim that a host sensor exists (SensorManager still returns no sensor).
  public static final int TYPE_ACCELEROMETER = 1;
  public int getType() {
    throw new UnsupportedOperationException("No host Android sensor adapter");
  }
}
