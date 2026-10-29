package android.media.audiofx;

/** Android's default five-band equalizer; it keeps its levels but the host mixer does not apply them (see AudioEffect). */
public class Equalizer extends AudioEffect {
  public static final int PARAM_NUM_BANDS = 0, PARAM_LEVEL_RANGE = 1, PARAM_BAND_LEVEL = 2, PARAM_CENTER_FREQ = 3,
      PARAM_BAND_FREQ_RANGE = 4, PARAM_GET_BAND = 5, PARAM_CURRENT_PRESET = 6, PARAM_GET_NUM_OF_PRESETS = 7,
      PARAM_GET_PRESET_NAME = 8, PARAM_STRING_SIZE_MAX = 32;
  private static final int[] CENTERS = {60000, 230000, 910000, 3600000, 14000000};  // milliHertz
  private static final int[][] RANGES = {{30000, 120000}, {120001, 460000}, {460001, 1800000},
      {1800001, 7000000}, {7000001, 20000000}};
  private static final String[] PRESETS = {"Normal", "Classical", "Dance", "Flat", "Folk", "Heavy Metal", "Hip Hop",
      "Jazz", "Pop", "Rock"};
  private final short[] levels = new short[CENTERS.length];
  private short preset;

  public Equalizer(int priority, int audioSession) { super(EFFECT_TYPE_EQUALIZER, "Equalizer", EFFECT_INSERT); }
  public short getNumberOfBands() { checkState(); return (short) CENTERS.length; }
  public short[] getBandLevelRange() { checkState(); return new short[] {-1500, 1500}; }
  public void setBandLevel(short band, short level) { checkState(); levels[band] = level; }
  public short getBandLevel(short band) { checkState(); return levels[band]; }
  public int getCenterFreq(short band) { checkState(); return CENTERS[band]; }
  public int[] getBandFreqRange(short band) { checkState(); return RANGES[band].clone(); }
  public short getBand(int frequency) {
    checkState();
    for (short i = 0; i < RANGES.length; ++i)
      if (frequency <= RANGES[i][1]) return i;
    return (short) (RANGES.length - 1);
  }
  public short getCurrentPreset() { checkState(); return preset; }
  public void usePreset(short value) {
    checkState();
    if (value < 0 || value >= PRESETS.length) throw new IllegalArgumentException("usePreset() invalid preset");
    preset = value;
  }
  public short getNumberOfPresets() { checkState(); return (short) PRESETS.length; }
  public String getPresetName(short value) { return value >= 0 && value < PRESETS.length ? PRESETS[value] : ""; }
}
