package android.media;
// Hidden AOSP class that older audio engines still reflect on: FMOD Ex's
// OpenSL ES output reads the primary output's rate and period from it and
// refuses to open the device when the class is missing.
public class AudioSystem {
  public static final int SUCCESS = 0, ERROR = -1;
  public static final int STREAM_VOICE_CALL = 0, STREAM_SYSTEM = 1, STREAM_RING = 2,
                          STREAM_MUSIC = 3, STREAM_ALARM = 4, STREAM_NOTIFICATION = 5;
  private AudioSystem() {}
  // Without an open host device Android still answers with the mixer's own
  // 48 kHz and its 5 ms period, as AudioManager.getProperty does.
  static int primaryOutput(int index) {
    int value = com.venture.runtime.RuntimeEnvironment.host().audioOutputProperties()[index];
    return value > 0 ? value : index == 0 ? 48000 : 240;
  }
  public static int getPrimaryOutputSamplingRate() { return primaryOutput(0); }
  public static int getPrimaryOutputFrameCount() { return primaryOutput(1); }
}
