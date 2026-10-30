package android.media;

/** AOSP android.media.AudioFocusRequest (API 26). */
public final class AudioFocusRequest {
  private final int focusGain;
  private final AudioAttributes attributes;
  private final AudioManager.OnAudioFocusChangeListener listener;
  private final android.os.Handler handler;
  private final boolean pauseWhenDucked, delayedFocus, forceDucking;
  private AudioFocusRequest(Builder b) {
    focusGain = b.focusGain; attributes = b.attributes; listener = b.listener; handler = b.handler;
    pauseWhenDucked = b.pauseWhenDucked; delayedFocus = b.delayedFocus; forceDucking = b.forceDucking;
  }
  public int getFocusGain() { return focusGain; }
  public AudioAttributes getAudioAttributes() { return attributes; }
  public boolean willPauseWhenDucked() { return pauseWhenDucked; }
  public boolean acceptsDelayedFocusGain() { return delayedFocus; }
  AudioManager.OnAudioFocusChangeListener listener() { return listener; }
  public static final class Builder {
    private int focusGain;
    private AudioAttributes attributes = new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).build();
    private AudioManager.OnAudioFocusChangeListener listener;
    private android.os.Handler handler;
    private boolean pauseWhenDucked, delayedFocus, forceDucking;
    public Builder(int focusGain) { setFocusGain(focusGain); }
    public Builder(AudioFocusRequest request) {
      focusGain = request.focusGain; attributes = request.attributes; listener = request.listener; handler = request.handler;
      pauseWhenDucked = request.pauseWhenDucked; delayedFocus = request.delayedFocus; forceDucking = request.forceDucking;
    }
    public Builder setFocusGain(int gain) {
      if (gain < AudioManager.AUDIOFOCUS_GAIN || gain > AudioManager.AUDIOFOCUS_GAIN_TRANSIENT_EXCLUSIVE)
        throw new IllegalArgumentException("Illegal audio focus gain type " + gain);
      focusGain = gain;
      return this;
    }
    public Builder setOnAudioFocusChangeListener(AudioManager.OnAudioFocusChangeListener l) {
      listener = java.util.Objects.requireNonNull(l, "Illegal null focus listener");
      return this;
    }
    public Builder setOnAudioFocusChangeListener(AudioManager.OnAudioFocusChangeListener l, android.os.Handler h) {
      listener = java.util.Objects.requireNonNull(l, "Illegal null focus listener");
      handler = java.util.Objects.requireNonNull(h, "Illegal null Handler");
      return this;
    }
    public Builder setAudioAttributes(AudioAttributes value) {
      attributes = java.util.Objects.requireNonNull(value, "Illegal null AudioAttributes");
      return this;
    }
    public Builder setWillPauseWhenDucked(boolean value) { pauseWhenDucked = value; return this; }
    public Builder setAcceptsDelayedFocusGain(boolean value) { delayedFocus = value; return this; }
    public Builder setForceDucking(boolean value) { forceDucking = value; return this; }
    public AudioFocusRequest build() {
      if ((delayedFocus || pauseWhenDucked) && listener == null)
        throw new IllegalStateException("Delayed focus and pause on duck need a focus listener");
      return new AudioFocusRequest(this);
    }
  }
}
