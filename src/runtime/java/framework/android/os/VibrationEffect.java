package android.os;

/** AOSP android.os.VibrationEffect: a one-shot vibration description (VibrationEffect.OneShot). */
public abstract class VibrationEffect {
  public static final int DEFAULT_AMPLITUDE = -1;
  VibrationEffect() {}
  public static VibrationEffect createOneShot(long milliseconds, int amplitude) {
    var effect = new OneShot(milliseconds, amplitude);
    effect.validate();
    return effect;
  }
  public abstract long getDuration();
  public abstract void validate();

  static final class OneShot extends VibrationEffect {
    private final long duration;
    private final int amplitude;
    OneShot(long duration, int amplitude) {
      this.duration = duration;
      this.amplitude = amplitude;
    }
    @Override public long getDuration() { return duration; }
    public int getAmplitude() { return amplitude; }
    @Override public void validate() {
      if (amplitude < -1 || amplitude == 0 || amplitude > 255)
        throw new IllegalArgumentException("amplitude must either be DEFAULT_AMPLITUDE, "
            + "or between 1 and 255 inclusive (amplitude=" + amplitude + ")");
      if (duration <= 0) throw new IllegalArgumentException("duration must be positive (duration=" + duration + ")");
    }
  }
}
