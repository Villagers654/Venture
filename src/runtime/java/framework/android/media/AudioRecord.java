package android.media;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/** Recording from the host's default input through the native mixer (the AudioTrack bridge). */
public class AudioRecord {
  public static final int STATE_UNINITIALIZED = 0, STATE_INITIALIZED = 1, RECORDSTATE_STOPPED = 1,
      RECORDSTATE_RECORDING = 3, READ_BLOCKING = 0, READ_NON_BLOCKING = 1, SUCCESS = 0, ERROR = -1,
      ERROR_BAD_VALUE = -2, ERROR_INVALID_OPERATION = -3, ERROR_DEAD_OBJECT = -6;
  private static int nextSession = 1;
  private volatile long handle;
  private volatile int recordingState = RECORDSTATE_STOPPED;
  private final int source, sampleRate, channelCount, channelMask, format, capacityFrames, session;

  public AudioRecord(int source, int rate, int channels, int encoding, int bytes) {
    this.source = source;
    channelCount = channelCount(channels);
    int sampleBytes = sampleBytes(encoding);
    if (rate < 8000 || rate > 192000 || channelCount == 0 || sampleBytes == 0 || bytes <= 0)
      throw new IllegalArgumentException("Unsupported PCM AudioRecord configuration");
    sampleRate = rate;
    channelMask = channelCount == 1 ? AudioFormat.CHANNEL_IN_MONO : AudioFormat.CHANNEL_IN_STEREO;
    format = encoding == AudioFormat.ENCODING_DEFAULT ? AudioFormat.ENCODING_PCM_16BIT : encoding;
    capacityFrames = Math.max(1, bytes / (sampleBytes * channelCount));
    synchronized (AudioRecord.class) { session = nextSession++; }
    handle = nativeOpen(rate, channelCount, format, capacityFrames);
  }
  private static int channelCount(int mask) {
    return switch (mask) { case 1, 2, AudioFormat.CHANNEL_IN_MONO -> 1; case 3, AudioFormat.CHANNEL_IN_STEREO -> 2; default -> 0; };
  }
  private static int sampleBytes(int format) {
    return switch (format) { case 1, 2 -> 2; case 3 -> 1; case 4 -> 4; default -> 0; };
  }
  public static int getMinBufferSize(int rate, int channels, int encoding) {
    int count = channelCount(channels), bytes = sampleBytes(encoding);
    if (count == 0 || bytes == 0 || rate < 8000 || rate > 192000) return ERROR_BAD_VALUE;
    return Math.max(1, rate / 50) * count * bytes;  // 20 ms
  }

  public static class Builder {
    private int source = 1, bytes;  // MediaRecorder.AudioSource.MIC
    private AudioFormat format;
    public Builder setAudioSource(int value) { source = value; return this; }
    public Builder setAudioFormat(AudioFormat value) { format = value; return this; }
    public Builder setBufferSizeInBytes(int value) { bytes = value; return this; }
    public Builder setContext(android.content.Context context) { return this; }
    public AudioRecord build() {
      int rate = format == null || format.getSampleRate() == 0 ? 48000 : format.getSampleRate();
      int mask = format == null || format.getChannelMask() == 0 ? AudioFormat.CHANNEL_IN_MONO : format.getChannelMask();
      int encoding = format == null || format.getEncoding() == AudioFormat.ENCODING_DEFAULT
          ? AudioFormat.ENCODING_PCM_16BIT : format.getEncoding();
      int size = bytes > 0 ? bytes : getMinBufferSize(rate, mask, encoding);
      return new AudioRecord(source, rate, mask, encoding, size);
    }
  }

  public int getState() { return handle != 0 ? STATE_INITIALIZED : STATE_UNINITIALIZED; }
  public int getRecordingState() { return recordingState; }
  public int getSampleRate() { return sampleRate; }
  public int getChannelCount() { return channelCount; }
  public int getChannelConfiguration() { return channelMask; }
  public int getAudioFormat() { return format; }
  public int getAudioSource() { return source; }
  public int getAudioSessionId() { return session; }
  public int getBufferSizeInFrames() { return capacityFrames; }
  public AudioFormat getFormat() {
    return new AudioFormat.Builder().setSampleRate(sampleRate).setChannelMask(channelMask).setEncoding(format).build();
  }
  public AudioDeviceInfo getRoutedDevice() { return null; }
  public boolean setPreferredDevice(AudioDeviceInfo device) { return device == null; }

  public synchronized void startRecording() {
    long current = handle;
    if (current == 0 || nativeControl(current, 6) < 0) throw new IllegalStateException("AudioRecord unavailable");
    recordingState = RECORDSTATE_RECORDING;
  }
  public synchronized void stop() {
    long current = handle;
    if (current == 0) throw new IllegalStateException("AudioRecord unavailable");
    nativeControl(current, 7);
    recordingState = RECORDSTATE_STOPPED;
  }
  public void release() {
    long current;
    synchronized (this) { current = handle; handle = 0; recordingState = RECORDSTATE_STOPPED; }
    if (current != 0) nativeControl(current, 0);
  }

  public int read(byte[] data, int offset, int size) { return read(data, offset, size, READ_BLOCKING); }
  public int read(byte[] data, int offset, int size, int mode) {
    if (handle == 0) return ERROR_INVALID_OPERATION;
    if (data == null || offset < 0 || size < 0 || offset > data.length - size || mode < 0 || mode > 1)
      return ERROR_BAD_VALUE;
    return nativeRead(handle, data, offset, size, mode == READ_BLOCKING);
  }
  public int read(short[] data, int offset, int size) { return read(data, offset, size, READ_BLOCKING); }
  public int read(short[] data, int offset, int size, int mode) {
    if (format != AudioFormat.ENCODING_PCM_16BIT || data == null || offset < 0 || size < 0 || offset > data.length - size)
      return ERROR_BAD_VALUE;
    byte[] bytes = new byte[Math.multiplyExact(size, 2)];
    int result = read(bytes, 0, bytes.length, mode);
    if (result > 0) ByteBuffer.wrap(bytes, 0, result).order(ByteOrder.LITTLE_ENDIAN).asShortBuffer().get(data, offset, result / 2);
    return result < 0 ? result : result / 2;
  }
  public int read(float[] data, int offset, int size, int mode) {
    if (format != AudioFormat.ENCODING_PCM_FLOAT || data == null || offset < 0 || size < 0 || offset > data.length - size)
      return ERROR_BAD_VALUE;
    byte[] bytes = new byte[Math.multiplyExact(size, 4)];
    int result = read(bytes, 0, bytes.length, mode);
    if (result > 0) ByteBuffer.wrap(bytes, 0, result).order(ByteOrder.LITTLE_ENDIAN).asFloatBuffer().get(data, offset, result / 4);
    return result < 0 ? result : result / 4;
  }
  public int read(ByteBuffer buffer, int size) { return read(buffer, size, READ_BLOCKING); }
  public int read(ByteBuffer buffer, int size, int mode) {
    if (buffer == null || size < 0 || size > buffer.capacity()) return ERROR_BAD_VALUE;
    byte[] bytes = new byte[size];
    int result = read(bytes, 0, size, mode);
    // Android writes from the buffer's start and leaves its position alone.
    if (result > 0) buffer.duplicate().clear().put(bytes, 0, result);
    return result;
  }

  private static native long nativeOpen(int rate, int channels, int format, int frames);
  private static native int nativeControl(long handle, int operation);
  private static native int nativeRead(long handle, byte[] bytes, int offset, int size, boolean blocking);
}
