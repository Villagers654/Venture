package android.media;

/** Demuxer over the host extractor that also backs AMediaExtractor. */
public final class MediaExtractor {
  public static final int SEEK_TO_PREVIOUS_SYNC = 0, SEEK_TO_NEXT_SYNC = 1, SEEK_TO_CLOSEST_SYNC = 2;
  public static final int SAMPLE_FLAG_SYNC = 1, SAMPLE_FLAG_ENCRYPTED = 2, SAMPLE_FLAG_PARTIAL_FRAME = 4;
  private static native long nativeOpen(String path, java.io.FileDescriptor fd, long offset, long length);
  private static native String nativeTrack(long peer, int index, long[] values);
  private static native byte[] nativeConfig(long peer, int index);
  private static native long nativeControl(long peer, int operation, long value, int mode);
  private static native int nativeRead(long peer, byte[] out, int offset);
  private static native void nativeRelease(long peer);
  private long peer;
  private int tracks;

  /** A track's MediaFormat from the host's {width,height,sample-rate,channel-count,durationUs}. */
  static MediaFormat format(String mime, long[] values) {
    MediaFormat format = new MediaFormat();
    format.setString(MediaFormat.KEY_MIME, mime);
    if (values[0] > 0) format.setInteger(MediaFormat.KEY_WIDTH, (int)values[0]);
    if (values[1] > 0) format.setInteger(MediaFormat.KEY_HEIGHT, (int)values[1]);
    if (values[2] > 0) format.setInteger(MediaFormat.KEY_SAMPLE_RATE, (int)values[2]);
    if (values[3] > 0) format.setInteger(MediaFormat.KEY_CHANNEL_COUNT, (int)values[3]);
    if (values[4] > 0) format.setLong(MediaFormat.KEY_DURATION, values[4]);
    return format;
  }
  private synchronized void open(String path, java.io.FileDescriptor fd, long offset, long length) throws java.io.IOException {
    if (peer != 0) { nativeRelease(peer); peer = 0; tracks = 0; }
    long value;
    try { value = nativeOpen(path, fd, offset, length); }
    catch (UnsatisfiedLinkError missing) { throw new java.io.IOException("Host media backend unavailable", missing); }
    if (value == 0) throw new java.io.IOException("Failed to instantiate extractor.");
    peer = value;
    while (nativeTrack(peer, tracks, new long[5]) != null) tracks++;
  }
  private long live() {
    if (peer == 0) throw new IllegalStateException("MediaExtractor has no data source");
    return peer;
  }
  public void setDataSource(String path) throws java.io.IOException {
    java.util.Objects.requireNonNull(path); open(path, null, 0, Long.MAX_VALUE);
  }
  public void setDataSource(String path, java.util.Map<String,String> headers) throws java.io.IOException {
    if (path.contains("://") && !path.startsWith("file://")) throw new java.io.IOException("Media network protocols unavailable");
    setDataSource(path.startsWith("file://") ? path.substring(7) : path);
  }
  public void setDataSource(android.content.Context context, android.net.Uri uri, java.util.Map<String,String> headers) throws java.io.IOException {
    java.util.Objects.requireNonNull(uri);
    if (uri.getScheme() == null || "file".equals(uri.getScheme())) setDataSource(uri.getPath());
    else throw new java.io.IOException("Media URI provider unavailable");
  }
  public void setDataSource(java.io.FileDescriptor fd) throws java.io.IOException { setDataSource(fd, 0, Long.MAX_VALUE); }
  public void setDataSource(java.io.FileDescriptor fd, long offset, long length) throws java.io.IOException {
    java.util.Objects.requireNonNull(fd);
    if (!fd.valid()) throw new java.io.IOException("Invalid media file descriptor");
    if (offset < 0 || length < 0) throw new IllegalArgumentException("Invalid media region");
    open(null, fd, offset, length);
  }
  public void setDataSource(android.content.res.AssetFileDescriptor afd) throws java.io.IOException {
    long length = afd.getDeclaredLength();
    setDataSource(afd.getFileDescriptor(), afd.getStartOffset(), length < 0 ? Long.MAX_VALUE : length);
  }
  public synchronized int getTrackCount() { return peer == 0 ? 0 : tracks; }
  public synchronized MediaFormat getTrackFormat(int index) {
    long[] values = new long[5];
    String mime = nativeTrack(live(), index, values);
    if (mime == null) throw new IllegalArgumentException("Invalid track index " + index);
    MediaFormat format = format(mime, values);
    byte[] config = nativeConfig(peer, index);
    if (config != null) format.setByteBuffer("csd-0", java.nio.ByteBuffer.wrap(config));
    return format;
  }
  private void select(int index, int operation) {
    if (index < 0 || index >= getTrackCount()) throw new IllegalArgumentException("Invalid track index " + index);
    if (nativeControl(live(), operation, index, 0) != 0) throw new IllegalStateException("Track selection failed");
  }
  public synchronized void selectTrack(int index) { select(index, 0); }
  public synchronized void unselectTrack(int index) { select(index, 1); }
  public synchronized boolean advance() { return peer != 0 && nativeControl(peer, 2, 0, 0) != 0; }
  public synchronized void seekTo(long timeUs, int mode) {
    if (mode < SEEK_TO_PREVIOUS_SYNC || mode > SEEK_TO_CLOSEST_SYNC) throw new IllegalArgumentException("Invalid seek mode");
    nativeControl(live(), 3, timeUs, mode);
  }
  public synchronized long getSampleTime() { return peer == 0 ? -1 : nativeControl(peer, 4, 0, 0); }
  public synchronized int getSampleTrackIndex() { return peer == 0 ? -1 : (int)nativeControl(peer, 5, 0, 0); }
  public synchronized int getSampleFlags() { return peer == 0 ? -1 : (int)nativeControl(peer, 6, 0, 0); }
  public synchronized long getSampleSize() { return peer == 0 ? -1 : nativeControl(peer, 7, 0, 0); }
  public synchronized long getCachedDuration() { return -1; }
  public synchronized boolean hasCacheReachedEndOfStream() { return true; }
  public synchronized int readSampleData(java.nio.ByteBuffer buffer, int offset) {
    long size = nativeControl(live(), 7, 0, 0);
    if (size < 0) return -1;
    if (offset < 0 || offset + size > buffer.capacity()) throw new IllegalArgumentException("Sample buffer too small");
    if (buffer.hasArray() && !buffer.isReadOnly()) {
      size = nativeRead(peer, buffer.array(), buffer.arrayOffset() + offset);
    } else {
      byte[] bytes = new byte[(int)size];
      size = nativeRead(peer, bytes, 0);
      if (size >= 0) { buffer.limit(buffer.capacity()); buffer.position(offset); buffer.put(bytes, 0, (int)size); }
    }
    if (size < 0) return -1;
    // AOSP leaves the sample between position and limit.
    buffer.limit(offset + (int)size); buffer.position(offset);
    return (int)size;
  }
  public synchronized void release() { if (peer != 0) nativeRelease(peer); peer = 0; tracks = 0; }
}
