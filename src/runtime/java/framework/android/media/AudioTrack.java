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
  private final int sampleRate, channelCount, format, capacityFrames;
  public AudioTrack(int stream, int rate, int channels, int encoding, int bytes, int mode) {
    this(stream, rate, channels, encoding, bytes, mode, 0);
  }
  public AudioTrack(int stream, int rate, int channels, int encoding, int bytes, int mode, int session) {
    if (mode != MODE_STREAM) throw new UnsupportedOperationException("Static AudioTrack unavailable");
    if (session != 0) throw new UnsupportedOperationException("Explicit audio sessions unavailable");
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
