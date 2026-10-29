package android.media;

/** PCM format constants and the API 21 builder. */
public final class AudioFormat {
  public static final int ENCODING_INVALID = 0, ENCODING_DEFAULT = 1, ENCODING_PCM_16BIT = 2,
      ENCODING_PCM_8BIT = 3, ENCODING_PCM_FLOAT = 4, ENCODING_PCM_24BIT_PACKED = 21, ENCODING_PCM_32BIT = 22;
  public static final int CHANNEL_INVALID = 0, CHANNEL_IN_DEFAULT = 1, CHANNEL_IN_LEFT = 4, CHANNEL_IN_RIGHT = 8,
      CHANNEL_IN_FRONT = 16, CHANNEL_IN_MONO = CHANNEL_IN_FRONT, CHANNEL_IN_STEREO = CHANNEL_IN_LEFT | CHANNEL_IN_RIGHT,
      CHANNEL_OUT_DEFAULT = 1, CHANNEL_OUT_FRONT_LEFT = 4, CHANNEL_OUT_FRONT_RIGHT = 8,
      CHANNEL_OUT_MONO = CHANNEL_OUT_FRONT_LEFT, CHANNEL_OUT_STEREO = CHANNEL_OUT_FRONT_LEFT | CHANNEL_OUT_FRONT_RIGHT,
      CHANNEL_CONFIGURATION_DEFAULT = 1, CHANNEL_CONFIGURATION_MONO = 2, CHANNEL_CONFIGURATION_STEREO = 3;
  public static final int SAMPLE_RATE_UNSPECIFIED = 0;
  private final int encoding, sampleRate, channelMask;

  private AudioFormat(int encoding, int sampleRate, int channelMask) {
    this.encoding = encoding;
    this.sampleRate = sampleRate;
    this.channelMask = channelMask;
  }
  public int getEncoding() { return encoding; }
  public int getSampleRate() { return sampleRate; }
  public int getChannelMask() { return channelMask; }
  public int getChannelCount() { return Integer.bitCount(channelMask); }
  public int getFrameSizeInBytes() { return Math.max(1, getChannelCount()) * bytesPerSample(encoding); }

  static int bytesPerSample(int encoding) {
    return switch (encoding) { case ENCODING_PCM_8BIT -> 1; case ENCODING_PCM_24BIT_PACKED -> 3;
      case ENCODING_PCM_FLOAT, ENCODING_PCM_32BIT -> 4; default -> 2; };
  }

  public static class Builder {
    private int encoding = ENCODING_DEFAULT, sampleRate = SAMPLE_RATE_UNSPECIFIED, channelMask = CHANNEL_INVALID;
    public Builder() {}
    public Builder(AudioFormat format) {
      encoding = format.encoding;
      sampleRate = format.sampleRate;
      channelMask = format.channelMask;
    }
    public Builder setEncoding(int value) { encoding = value; return this; }
    public Builder setSampleRate(int value) { sampleRate = value; return this; }
    public Builder setChannelMask(int value) { channelMask = value; return this; }
    public AudioFormat build() { return new AudioFormat(encoding, sampleRate, channelMask); }
  }
}
