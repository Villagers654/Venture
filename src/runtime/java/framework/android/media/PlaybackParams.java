package android.media;

/** AOSP android.media.PlaybackParams: playback speed and pitch. */
public final class PlaybackParams {
  public static final int AUDIO_FALLBACK_MODE_DEFAULT = 0, AUDIO_FALLBACK_MODE_MUTE = 1, AUDIO_FALLBACK_MODE_FAIL = 2;
  private static final int SET_SPEED = 1, SET_PITCH = 2, SET_AUDIO_FALLBACK_MODE = 4;
  private int set;
  private int audioFallbackMode = AUDIO_FALLBACK_MODE_DEFAULT;
  private float pitch = 1.0f, speed = 1.0f;
  public PlaybackParams() {}
  public PlaybackParams allowDefaults() { set |= SET_SPEED | SET_PITCH | SET_AUDIO_FALLBACK_MODE; return this; }
  public PlaybackParams setAudioFallbackMode(int mode) { audioFallbackMode = mode; set |= SET_AUDIO_FALLBACK_MODE; return this; }
  public int getAudioFallbackMode() {
    if ((set & SET_AUDIO_FALLBACK_MODE) == 0) throw new IllegalStateException("audio fallback mode not set");
    return audioFallbackMode;
  }
  public PlaybackParams setPitch(float value) {
    if (value < 0.f) throw new IllegalArgumentException("pitch must not be negative");
    pitch = value; set |= SET_PITCH; return this;
  }
  public float getPitch() {
    if ((set & SET_PITCH) == 0) throw new IllegalStateException("pitch not set");
    return pitch;
  }
  public PlaybackParams setSpeed(float value) { speed = value; set |= SET_SPEED; return this; }
  public float getSpeed() {
    if ((set & SET_SPEED) == 0) throw new IllegalStateException("speed not set");
    return speed;
  }
}
