package android.hardware;
public class SensorEvent {
  public Sensor sensor;
  public int accuracy;
  public long timestamp;
  public final float[] values;
  public SensorEvent(int size) {
    values = new float[size];
  }
}
