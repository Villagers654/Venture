package android.media.audiofx;

/** Keeps its gain; the host mixer does not apply it (see AudioEffect). */
public class LoudnessEnhancer extends AudioEffect {
  public static final int PARAM_TARGET_GAIN_MB = 0;
  private float gain;
  public LoudnessEnhancer(int audioSession) { super(EFFECT_TYPE_LOUDNESS_ENHANCER, "Loudness Enhancer", EFFECT_INSERT); }
  public void setTargetGain(int millibels) { checkState(); gain = millibels; }
  public float getTargetGain() { checkState(); return gain; }
}
