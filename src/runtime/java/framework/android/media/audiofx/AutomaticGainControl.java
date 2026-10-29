package android.media.audiofx;

/** Capture pre-processing the host does not offer: unavailable, so create() returns null. */
public class AutomaticGainControl extends AudioEffect {
  private AutomaticGainControl() { super(EFFECT_TYPE_AGC, "Automatic Gain Control", EFFECT_PRE_PROCESSING); }
  public static boolean isAvailable() { return false; }
  public static AutomaticGainControl create(int audioSession) { return null; }
}
