package android.media;

/** AOSP android.media.Spatializer (API 32). Venture's host mixer has no spatializer effect. */
public class Spatializer {
  public static final int SPATIALIZER_IMMERSIVE_LEVEL_OTHER = -1, SPATIALIZER_IMMERSIVE_LEVEL_NONE = 0,
      SPATIALIZER_IMMERSIVE_LEVEL_MULTICHANNEL = 1;
  public interface OnSpatializerStateChangedListener {
    void onSpatializerEnabledChanged(Spatializer spat, boolean enabled);
    void onSpatializerAvailableChanged(Spatializer spat, boolean available);
  }
  Spatializer() {}
  public int getImmersiveAudioLevel() { return SPATIALIZER_IMMERSIVE_LEVEL_NONE; }
  public boolean isEnabled() { return false; }
  public boolean isAvailable() { return false; }
  public boolean isHeadTrackerAvailable() { return false; }
  public boolean canBeSpatialized(AudioAttributes attributes, AudioFormat format) { return false; }
  public void addOnSpatializerStateChangedListener(java.util.concurrent.Executor executor,
                                                   OnSpatializerStateChangedListener listener) {
    java.util.Objects.requireNonNull(executor);
    java.util.Objects.requireNonNull(listener);
  }
  public void removeOnSpatializerStateChangedListener(OnSpatializerStateChangedListener listener) {}
}
