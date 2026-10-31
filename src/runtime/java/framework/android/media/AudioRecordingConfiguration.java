package android.media;

/** One recording in progress; only this app's own recordings exist. */
public final class AudioRecordingConfiguration {
  private final int source, session;
  private final AudioFormat format;
  AudioRecordingConfiguration(AudioRecord record) {
    source = record.getAudioSource();
    session = record.getAudioSessionId();
    format = record.getFormat();
  }
  public int getClientAudioSource() { return source; }
  public int getAudioSource() { return source; }
  public int getClientAudioSessionId() { return session; }
  public AudioFormat getClientFormat() { return format; }
  public AudioFormat getFormat() { return format; }
  public AudioDeviceInfo getAudioDevice() { return null; }
  public boolean isClientSilenced() { return false; }
  public java.util.List<android.media.audiofx.AudioEffect.Descriptor> getClientEffects() { return java.util.List.of(); }
  public java.util.List<android.media.audiofx.AudioEffect.Descriptor> getEffects() { return java.util.List.of(); }
  public int getClientPortId() { return 0; }
  public int describeContents() { return 0; }
}
