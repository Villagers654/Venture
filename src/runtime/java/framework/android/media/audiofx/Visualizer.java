package android.media.audiofx;

/** Output capture for visualizations; the host mixer offers none, so captures are silence. */
public class Visualizer {
  public static final int STATE_UNINITIALIZED = 0, STATE_INITIALIZED = 1, STATE_ENABLED = 2,
      SCALING_MODE_NORMALIZED = 0, SCALING_MODE_AS_PLAYED = 1, MEASUREMENT_MODE_NONE = 0,
      MEASUREMENT_MODE_PEAK_RMS = 1, SUCCESS = 0, ERROR = -1, ALREADY_EXISTS = -2, ERROR_NO_INIT = -3,
      ERROR_BAD_VALUE = -4, ERROR_INVALID_OPERATION = -5, ERROR_NO_MEMORY = -6, ERROR_DEAD_OBJECT = -7;
  public interface OnDataCaptureListener {
    void onWaveFormDataCapture(Visualizer visualizer, byte[] waveform, int samplingRate);
    void onFftDataCapture(Visualizer visualizer, byte[] fft, int samplingRate);
  }
  public static final class MeasurementPeakRms {
    public int mPeak = -9600, mRms = -9600;
  }
  private int state = STATE_INITIALIZED, captureSize = 1024, scaling, measurement;

  public Visualizer(int audioSession) {}
  public static int[] getCaptureSizeRange() { return new int[] {128, 1024}; }
  public static int getMaxCaptureRate() { return 20000; }  // milliHertz
  private void check() {
    if (state == STATE_UNINITIALIZED) throw new IllegalStateException("Visualizer was released");
  }
  public int setEnabled(boolean enabled) { check(); state = enabled ? STATE_ENABLED : STATE_INITIALIZED; return SUCCESS; }
  public boolean getEnabled() { check(); return state == STATE_ENABLED; }
  public int setCaptureSize(int size) {
    check();
    if (state == STATE_ENABLED) return ERROR_INVALID_OPERATION;
    if (size < 128 || size > 1024 || Integer.bitCount(size) != 1) return ERROR_BAD_VALUE;
    captureSize = size;
    return SUCCESS;
  }
  public int getCaptureSize() { check(); return captureSize; }
  public int setScalingMode(int mode) { check(); scaling = mode; return SUCCESS; }
  public int getScalingMode() { check(); return scaling; }
  public int setMeasurementMode(int mode) { check(); measurement = mode; return SUCCESS; }
  public int getMeasurementMode() { check(); return measurement; }
  public int getSamplingRate() { check(); return 48000000; }  // milliHertz
  public int getWaveForm(byte[] waveform) {
    check();
    java.util.Arrays.fill(waveform, 0, Math.min(waveform.length, captureSize), (byte) 0x80);  // unsigned 8-bit silence
    return SUCCESS;
  }
  public int getFft(byte[] fft) {
    check();
    java.util.Arrays.fill(fft, 0, Math.min(fft.length, captureSize), (byte) 0);
    return SUCCESS;
  }
  public int getMeasurementPeakRms(MeasurementPeakRms out) { check(); out.mPeak = out.mRms = -9600; return SUCCESS; }
  public int setDataCaptureListener(OnDataCaptureListener listener, int rate, boolean waveform, boolean fft) {
    check();
    return SUCCESS;
  }
  public void release() { state = STATE_UNINITIALIZED; }
}
