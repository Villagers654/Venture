package android.media.audiofx;

/** Keeps its preset; the host mixer does not apply it (see AudioEffect). */
public class PresetReverb extends AudioEffect {
  public static final short PRESET_NONE = 0, PRESET_SMALLROOM = 1, PRESET_MEDIUMROOM = 2, PRESET_LARGEROOM = 3,
      PRESET_MEDIUMHALL = 4, PRESET_LARGEHALL = 5, PRESET_PLATE = 6;
  private short preset;
  public PresetReverb(int priority, int audioSession) { super(EFFECT_TYPE_PRESET_REVERB, "Preset Reverb", EFFECT_AUXILIARY); }
  public void setPreset(short value) { checkState(); preset = value; }
  public short getPreset() { checkState(); return preset; }
}
