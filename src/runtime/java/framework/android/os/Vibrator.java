package android.os;
public class Vibrator {
  public boolean hasVibrator() {
    return false;
  }
  public boolean hasAmplitudeControl() {
    return false;
  }
  public void vibrate(long milliseconds) {
    android.util.Log.w("Vibrator", "No host vibration device");
  }
  public void vibrate(VibrationEffect effect) {
    effect.validate();
    android.util.Log.w("Vibrator", "No host vibration device");
  }
  public void cancel() {}
}
