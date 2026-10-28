package android.os;
public class BatteryManager {
  public static final int BATTERY_PROPERTY_CAPACITY = 4;
  public int getIntProperty(int id) {
    return id == 4 ? 100 : Integer.MIN_VALUE;
  }
  public long getLongProperty(int id) {
    return Long.MIN_VALUE;
  }
  public boolean isCharging() {
    return true;
  }
}
