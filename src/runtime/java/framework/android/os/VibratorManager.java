package android.os;

import android.content.Context;

/** Android device vibrators are separate from OpenXR controller haptics. */
public class VibratorManager {
  private final Context context;
  public VibratorManager(Context context) {
    this.context = java.util.Objects.requireNonNull(context);
  }
  // AOSP android12-release SystemVibratorManager: absent vibrator service
  // yields no IDs and a NullVibrator for an unavailable device. Our existing
  // Vibrator implements that no-device behavior (hasVibrator() == false).
  public int[] getVibratorIds() { return new int[0]; }
  public Vibrator getVibrator(int vibratorId) { return getDefaultVibrator(); }
  public Vibrator getDefaultVibrator() {
    return (Vibrator) context.getSystemService(Context.VIBRATOR_SERVICE);
  }
}
