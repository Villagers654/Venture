package android.media.audiofx;

/** Keeps its strength; the host mixer does not apply it (see AudioEffect). */
public class BassBoost extends AudioEffect {
  public static final int PARAM_STRENGTH_SUPPORTED = 0, PARAM_STRENGTH = 1;
  private short strength;
  public BassBoost(int priority, int audioSession) { super(EFFECT_TYPE_BASS_BOOST, "Bass Boost", EFFECT_INSERT); }
  public boolean getStrengthSupported() { return true; }
  public void setStrength(short value) { checkState(); strength = value; }
  public short getRoundedStrength() { checkState(); return strength; }
}
