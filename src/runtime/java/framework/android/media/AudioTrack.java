package android.media;

/** Streaming PCM backed by the same native mixer as guest AAudio/OpenSL. */
public class AudioTrack {
  static {
    Runtime.getRuntime().addShutdownHook(new Thread(AudioTrack::nativeShutdown, "Venture-audio-stop"));
  }
  public static final int MODE_STATIC = 0, MODE_STREAM = 1, STATE_UNINITIALIZED = 0,
      STATE_INITIALIZED = 1, STATE_NO_STATIC_DATA = 2, PLAYSTATE_STOPPED = 1,
      PLAYSTATE_PAUSED = 2, PLAYSTATE_PLAYING = 3, WRITE_BLOCKING = 0, WRITE_NON_BLOCKING = 1,
      SUCCESS = 0, ERROR = -1, ERROR_BAD_VALUE = -2, ERROR_INVALID_OPERATION = -3, ERROR_DEAD_OBJECT = -6;
  private volatile long handle;
  private volatile int playState = PLAYSTATE_STOPPED;
  private final int sampleRate, channelCount, format, capacityFrames, sessionId;
  private static final java.util.concurrent.atomic.AtomicInteger nextSession = new java.util.concurrent.atomic.AtomicInteger(4096);
  private PlaybackParams playbackParams = new PlaybackParams().setSpeed(1f).setPitch(1f);
  public AudioTrack(int stream, int rate, int channels, int encoding, int bytes, int mode) {
    this(stream, rate, channels, encoding, bytes, mode, 0);
  }
  public AudioTrack(AudioAttributes attributes, AudioFormat format, int bytes, int mode, int session) {
    this(attributes.getVolumeControlStream(), format.getSampleRate() != 0 ? format.getSampleRate() : 48000,
        format.getChannelMask() != 0 ? format.getChannelMask() : AudioFormat.CHANNEL_OUT_STEREO,
        format.getEncoding() != AudioFormat.ENCODING_DEFAULT ? format.getEncoding() : AudioFormat.ENCODING_PCM_16BIT,
        bytes, mode, session);
  }
  public AudioTrack(int stream, int rate, int channels, int encoding, int bytes, int mode, int session) {
    if (mode != MODE_STREAM) throw new UnsupportedOperationException("Static AudioTrack unavailable");
    // One host mixer: a session id only names the track for effects and routing queries.
    sessionId = session > 0 ? session : nextSession.getAndIncrement() << 3 | 1;
    channelCount = channels == 4 || channels == 2 ? 1 : channels == 12 || channels == 3 ? 2 : 0;
    int sampleBytes = sampleBytes(encoding);
    if (rate < 8000 || rate > 192000 || channelCount == 0 || sampleBytes == 0 || bytes <= 0
        || bytes % (sampleBytes * channelCount) != 0)
      throw new IllegalArgumentException("Unsupported PCM AudioTrack configuration");
    sampleRate = rate; format = encoding; capacityFrames = bytes / (sampleBytes * channelCount);
    handle = nativeOpen(rate, channelCount, encoding, capacityFrames);
  }
  private static int sampleBytes(int format) {
    return switch (format) { case 2 -> 2; case 3 -> 1; case 4, 22 -> 4; case 21 -> 3; default -> 0; };
  }
  public static int getMinBufferSize(int rate, int channels, int encoding) {
    int count = channels == 4 || channels == 2 ? 1 : channels == 12 || channels == 3 ? 2 : 0;
    int bytes = sampleBytes(encoding);
    if (count == 0 || bytes == 0 || rate < 8000 || rate > 192000) return ERROR_BAD_VALUE;
    return Math.max(1, rate / 50) * count * bytes;
  }
  public static int getNativeOutputSampleRate(int stream) { return 48000; }
  public int getSampleRate() { return sampleRate; }
  public int getChannelCount() { return channelCount; }
  public int getAudioFormat() { return format; }
  public int getBufferSizeInFrames() { return capacityFrames; }
  public int getBufferCapacityInFrames() { return capacityFrames; }
  public int getState() { return handle != 0 ? STATE_INITIALIZED : STATE_UNINITIALIZED; }
  public int getPlayState() { return playState; }
  private void control(int operation) {
    long current = handle;
    if (current == 0 || nativeControl(current, operation) < 0) throw new IllegalStateException("AudioTrack unavailable");
  }
  public synchronized void play() { control(1); playState = PLAYSTATE_PLAYING; }
  public synchronized void pause() { control(2); playState = PLAYSTATE_PAUSED; }
  // Native stop only requests drain. No endpoint progress is awaited here.
  public synchronized void stop() { control(3); playState = PLAYSTATE_STOPPED; }
  public synchronized void flush() { if (handle != 0 && playState != PLAYSTATE_PLAYING) control(4); }
  public void release() {
    long current;
    synchronized (this) { current = handle; handle = 0; playState = PLAYSTATE_STOPPED; }
    // Joining the native worker must not retain the Java control monitor.
    if (current != 0) nativeControl(current, 0);
  }
  public int getPlaybackHeadPosition() { return handle == 0 ? 0 : nativeControl(handle, 5); }
  public int write(byte[] data, int offset, int size) { return write(data, offset, size, WRITE_BLOCKING); }
  public int write(byte[] data, int offset, int size, int mode) {
    if (handle == 0) return ERROR_INVALID_OPERATION;
    if (data == null || offset < 0 || size < 0 || offset > data.length - size || mode < 0 || mode > 1)
      return ERROR_BAD_VALUE;
    return nativeWrite(handle, data, offset, size, mode == WRITE_BLOCKING);
  }
  public int write(short[] data, int offset, int size) {
    if (format != 2 || data == null || offset < 0 || size < 0 || offset > data.length - size) return ERROR_BAD_VALUE;
    byte[] bytes = new byte[Math.multiplyExact(size, 2)];
    java.nio.ByteBuffer.wrap(bytes).order(java.nio.ByteOrder.LITTLE_ENDIAN).asShortBuffer().put(data, offset, size);
    int result = write(bytes, 0, bytes.length);
    return result < 0 ? result : result / 2;
  }
  public int write(java.nio.ByteBuffer data, int size, int mode) {
    if (handle == 0) return ERROR_INVALID_OPERATION;
    if (data == null || size < 0 || size > data.remaining() || mode < 0 || mode > 1) return ERROR_BAD_VALUE;
    byte[] bytes;
    int offset;
    if (data.hasArray()) {
      bytes = data.array();
      offset = data.arrayOffset() + data.position();
    } else {
      bytes = new byte[size];
      data.duplicate().get(bytes);
      offset = 0;
    }
    int written = nativeWrite(handle, bytes, offset, size, mode == WRITE_BLOCKING);
    if (written > 0) data.position(data.position() + written);
    return written;
  }
  /** The timestamp is only meaningful for HW A/V sync tracks, which Venture does not create. */
  public int write(java.nio.ByteBuffer data, int size, int mode, long timestamp) { return write(data, size, mode); }
  public int write(float[] data, int offset, int size, int mode) {
    if (format != AudioFormat.ENCODING_PCM_FLOAT || data == null || offset < 0 || size < 0 || offset > data.length - size)
      return ERROR_BAD_VALUE;
    var bytes = java.nio.ByteBuffer.allocate(Math.multiplyExact(size, 4)).order(java.nio.ByteOrder.LITTLE_ENDIAN);
    bytes.asFloatBuffer().put(data, offset, size);
    int result = write(bytes.array(), 0, bytes.capacity(), mode);
    return result < 0 ? result : result / 4;
  }
  public int getAudioSessionId() { return sessionId; }
  /** No presentation timestamp from the host mixer; callers fall back to the playback head position. */
  public boolean getTimestamp(AudioTimestamp timestamp) { return false; }
  public synchronized PlaybackParams getPlaybackParams() { return playbackParams; }
  public synchronized void setPlaybackParams(PlaybackParams params) {
    // The host mixer plays at 1x; other rates are refused like an unsupported HAL rate.
    if (params.getSpeed() != 1f || params.getPitch() != 1f) throw new IllegalArgumentException("Unsupported playback rate");
    playbackParams = params;
  }
  public int attachAuxEffect(int effectId) { return effectId == 0 ? SUCCESS : ERROR_BAD_VALUE; }
  public int setAuxEffectSendLevel(float level) { return SUCCESS; }
  public boolean isOffloadedPlayback() { return false; }
  public static boolean isDirectPlaybackSupported(AudioFormat format, AudioAttributes attributes) { return false; }
  public void setOffloadDelayPadding(int delayInFrames, int paddingInFrames) {
    throw new IllegalStateException("Illegal use of delay/padding on non-offloaded track");
  }
  public void setOffloadEndOfStream() { throw new IllegalStateException("EOS not supported on non-offloaded track"); }
  public abstract static class StreamEventCallback {
    public StreamEventCallback() {}
    public void onTearDown(AudioTrack track) {}
    public void onPresentationEnded(AudioTrack track) {}
    public void onDataRequest(AudioTrack track, int sizeInFrames) {}
  }
  public void setLogSessionId(android.media.metrics.LogSessionId id) { java.util.Objects.requireNonNull(id); }
  public void registerStreamEventCallback(java.util.concurrent.Executor executor, StreamEventCallback callback) {
    throw new IllegalStateException("Cannot register StreamEventCallback on non-offloaded AudioTrack");
  }
  public void unregisterStreamEventCallback(StreamEventCallback callback) {}
  public int getUnderrunCount() { return 0; }
  public static final class Builder {
    private AudioAttributes attributes = new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).build();
    private AudioFormat format;
    private int bufferSize, mode = MODE_STREAM, sessionId;
    public Builder() {}
    public Builder setAudioAttributes(AudioAttributes value) { attributes = java.util.Objects.requireNonNull(value); return this; }
    public Builder setAudioFormat(AudioFormat value) { format = java.util.Objects.requireNonNull(value); return this; }
    public Builder setBufferSizeInBytes(int value) {
      if (value <= 0) throw new IllegalArgumentException("Invalid buffer size " + value);
      bufferSize = value;
      return this;
    }
    public Builder setTransferMode(int value) { mode = value; return this; }
    public Builder setSessionId(int value) { sessionId = value; return this; }
    public Builder setPerformanceMode(int value) { return this; }
    public Builder setOffloadedPlayback(boolean value) {
      if (value) throw new UnsupportedOperationException("Offloaded playback unavailable");
      return this;
    }
    public AudioTrack build() {
      var actual = format != null ? format : new AudioFormat.Builder().setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
          .setEncoding(AudioFormat.ENCODING_PCM_16BIT).build();
      int bytes = bufferSize;
      if (bytes == 0) {
        int rate = actual.getSampleRate() != 0 ? actual.getSampleRate() : 48000;
        bytes = Math.max(1, getMinBufferSize(rate, actual.getChannelMask(), actual.getEncoding()));
      }
      return new AudioTrack(attributes, actual, bytes, mode, sessionId);
    }
  }
  public int setStereoVolume(float left, float right) {
    if (handle == 0) return ERROR_INVALID_OPERATION;
    if (!Float.isFinite(left) || !Float.isFinite(right)) return ERROR_BAD_VALUE;
    return nativeVolume(handle, Math.max(0, Math.min(1, left)), Math.max(0, Math.min(1, right)));
  }
  public int setVolume(float volume) { return setStereoVolume(volume, volume); }
  private static native long nativeOpen(int rate, int channels, int format, int frames);
  private static native int nativeControl(long handle, int operation);
  private static native int nativeWrite(long handle, byte[] bytes, int offset, int size, boolean blocking);
  private static native int nativeVolume(long handle, float left, float right);
  private static native void nativeShutdown();
}
