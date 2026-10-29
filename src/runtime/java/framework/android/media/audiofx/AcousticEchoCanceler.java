package android.media.audiofx;

/** Capture pre-processing the host does not offer: unavailable, so create() returns null. */
public class AcousticEchoCanceler extends AudioEffect {
  private AcousticEchoCanceler() { super(EFFECT_TYPE_AEC, "Acoustic Echo Canceler", EFFECT_PRE_PROCESSING); }
  public static boolean isAvailable() { return false; }
  public static AcousticEchoCanceler create(int audioSession) { return null; }
}
