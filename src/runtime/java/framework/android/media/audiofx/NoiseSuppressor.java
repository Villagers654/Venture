package android.media.audiofx;

/** Capture pre-processing the host does not offer: unavailable, so create() returns null. */
public class NoiseSuppressor extends AudioEffect {
  private NoiseSuppressor() { super(EFFECT_TYPE_NS, "Noise Suppressor", EFFECT_PRE_PROCESSING); }
  public static boolean isAvailable() { return false; }
  public static NoiseSuppressor create(int audioSession) { return null; }
}
