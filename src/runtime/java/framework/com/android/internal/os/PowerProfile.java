package com.android.internal.os;

/** Hidden API that fingerprinting libraries read reflectively; a Quest 3 battery holds 5060 mAh. */
public class PowerProfile {
  public PowerProfile(android.content.Context context) {}
  public double getBatteryCapacity() { return 5060; }
  public double getAveragePower(String type) { return 0; }
  public double getAveragePower(String type, int level) { return 0; }
}
