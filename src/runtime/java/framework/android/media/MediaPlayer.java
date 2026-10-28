package android.media;

/** Audio player backed by the host's bounded MOV/AAC worker and existing mixer. */
public class MediaPlayer {
  public static final int MEDIA_ERROR_UNKNOWN = 1, MEDIA_ERROR_UNSUPPORTED = -1010;
  // AOSP media/java/android/media/MediaPlayer.java, Valid_and_Invalid_States:
  // construction/reset enter Idle, release enters End. No media is prepared by
  // construction. Queries in Idle must not require a decoder.
  private boolean released, error, fresh = true, looping;
  private long generation;
  private long peer;
  private boolean sourced, prepared, preparing, completionPosted, stopped;
  private static native long nativeSource(String path, java.io.FileDescriptor fd, long offset, long length);
  private static native int nativeControl(long peer, int operation, long value);
  private static native long nativeQuery(long peer, int operation);
  private static native void nativeVolume(long peer, float left, float right);
  private static native void nativeSurface(long peer, android.view.Surface surface);
  private static native void nativeRelease(long peer);
  private float leftVolume = 1, rightVolume = 1;
  private android.view.Surface surface;
  private android.view.SurfaceHolder display;
  private final android.os.Handler events;
  private OnErrorListener errorListener;
  private OnCompletionListener completionListener;
  private OnPreparedListener preparedListener;
  private OnVideoSizeChangedListener sizeListener;
  private OnSeekCompleteListener seekListener;
  private OnBufferingUpdateListener bufferingListener;
  private OnInfoListener infoListener;
  public MediaPlayer() {
    var looper = android.os.Looper.myLooper();
    if (looper == null) looper = android.os.Looper.getMainLooper();
    events = looper == null ? null : new android.os.Handler(looper);
  }
  private void live() {
    if (released) throw new IllegalStateException("MediaPlayer is released");
  }
  private void query() {
    live();
    if (error) throw new IllegalStateException("MediaPlayer is in Error state");
  }
  private void invalid(String operation) {
    live();
    android.util.Log.e("MediaPlayer", operation + " requires prepared media");
    // AOSP distinguishes a new Idle player (no callback/state change) from a
    // reset Idle player (Error + asynchronous onError). Reset/release cancel
    // pending events, including their completion-listener fallback.
    if (fresh) return;
    error = true;
    long token = generation;
    if (events != null) events.post(() -> {
      synchronized (this) {
        if (released || token != generation) return;
        boolean handled = errorListener != null && errorListener.onError(this, MEDIA_ERROR_UNKNOWN, -38);
        if (!handled && token == generation && !released && completionListener != null)
          completionListener.onCompletion(this);
      }
    });
  }
  public synchronized boolean isPlaying() { query(); return peer != 0 && prepared && nativeQuery(peer, 2) != 0; }
  public synchronized int getCurrentPosition() { query(); return peer == 0 ? 0 : (int)Math.min(Integer.MAX_VALUE, nativeQuery(peer, 1) / 1000); }
  public synchronized int getVideoWidth() { query(); return prepared ? (int)nativeQuery(peer, 5) : 0; }
  public synchronized int getVideoHeight() { query(); return prepared ? (int)nativeQuery(peer, 6) : 0; }
  public synchronized int getDuration() {
    if (!prepared && !stopped) { invalid("getDuration"); return -1; }
    query(); return (int)Math.min(Integer.MAX_VALUE, nativeQuery(peer, 0) / 1000);
  }
  public synchronized boolean isLooping() { live(); return looping; }
  public synchronized void setLooping(boolean value) { query(); looping = value; if (peer != 0) nativeControl(peer, 5, value ? 1 : 0); }
  public synchronized void setVolume(float left, float right) {
    query(); leftVolume = Math.max(0, Math.min(1, left)); rightVolume = Math.max(0, Math.min(1, right));
    if (peer != 0) nativeVolume(peer, leftVolume, rightVolume);
  }
  public synchronized void setSurface(android.view.Surface value) { live(); com.venture.runtime.RuntimeEnvironment.trace("MediaPlayer: setSurface valid=" + (value != null && value.isValid())); surface = value; display = null; if (peer != 0) nativeSurface(peer, value); }
  public synchronized void setDisplay(android.view.SurfaceHolder value) { live(); display = value; surface = null; if (peer != 0) nativeSurface(peer, value == null ? null : value.getSurface()); }
  private boolean control(String operation, int code, long value) {
    com.venture.runtime.RuntimeEnvironment.trace("MediaPlayer: " + operation + " value=" + value);
    if (!prepared) { invalid(operation); return false; }
    query();
    if (nativeControl(peer, code, value) != 0) {
      error = true; postError(generation, MEDIA_ERROR_UNSUPPORTED); return false;
    }
    return true;
  }
  private void postError(long token, int extra) {
    if (events != null) events.post(() -> {
      synchronized (this) {
        if (released || token != generation) return;
        boolean handled = errorListener != null && errorListener.onError(this, MEDIA_ERROR_UNKNOWN, extra);
        if (!handled && token == generation && !released && completionListener != null) completionListener.onCompletion(this);
      }
    });
  }
  private void monitor(long token) {
    if (events == null) return;
    events.postDelayed(() -> {
      synchronized (this) {
        if (released || token != generation || !prepared || error) return;
        if (nativeQuery(peer, 4) != 0) { error = true; postError(token, MEDIA_ERROR_UNSUPPORTED); return; }
        if (nativeQuery(peer, 3) != 0 && !looping) {
          if (!completionPosted) { completionPosted = true; if (completionListener != null) completionListener.onCompletion(this); }
          return;
        }
        if (nativeQuery(peer, 2) != 0) monitor(token);
      }
    }, 20);
  }
  public synchronized void start() { if (control("start", 1, 0)) { completionPosted = false; monitor(generation); } }
  public synchronized void pause() { control("pause", 2, 0); }
  public synchronized void stop() { if (control("stop", 3, 0)) { prepared = false; stopped = true; } }
  public synchronized void seekTo(int position) {
    if (control("seekTo", 4, Math.max(0, position) * 1000L) && events != null) {
      long token = generation;
      events.post(() -> { synchronized (this) { if (!released && token == generation && seekListener != null) seekListener.onSeekComplete(this); } });
    }
  }
  public synchronized void seekTo(long position, int mode) {
    if (mode < 0 || mode > 3) throw new IllegalArgumentException("Invalid seek mode");
    seekTo((int)Math.min(Integer.MAX_VALUE, Math.max(0, position)));
  }
  public synchronized void selectTrack(int index) {
    query(); if (!prepared || index != 0) throw new IllegalStateException("Audio track unavailable");
  }
  public synchronized void prepare() throws java.io.IOException {
    query();
    if (!sourced || prepared || preparing) throw new IllegalStateException("MediaPlayer has no available data source");
    com.venture.runtime.RuntimeEnvironment.trace("MediaPlayer: prepare surface=" + (surface != null) + " display=" + (display != null));
    nativeSurface(peer, display != null ? display.getSurface() : surface);
    if (nativeControl(peer, 0, 0) != 0) { error = true; throw new java.io.IOException("Unsupported audio media format"); }
    prepared = true; stopped = false;
  }
  public synchronized void prepareAsync() {
    query();
    if (!sourced || prepared || preparing) throw new IllegalStateException("MediaPlayer has no available data source");
    com.venture.runtime.RuntimeEnvironment.trace("MediaPlayer: prepareAsync surface=" + (surface != null) + " display=" + (display != null));
    nativeSurface(peer, display != null ? display.getSurface() : surface);
    preparing = true; long token = generation, id = peer;
    Thread worker = new Thread(() -> {
      int result;
      try { result = nativeControl(id, 0, 0); } catch (Exception failure) { result = MEDIA_ERROR_UNSUPPORTED; }
      synchronized (this) {
        if (released || token != generation) return;
        preparing = false;
        if (result != 0) { error = true; postError(token, MEDIA_ERROR_UNSUPPORTED); return; }
        prepared = true; stopped = false;
        if (events != null) events.post(() -> { synchronized (this) {
          if (released || token != generation) return;
          if (sizeListener != null && getVideoWidth() > 0) sizeListener.onVideoSizeChanged(this, getVideoWidth(), getVideoHeight());
          if (!released && token == generation && preparedListener != null) preparedListener.onPrepared(this);
        } });
      }
    }, "Venture-media-prepare");
    worker.setDaemon(true); worker.start();
  }
  private void source(String path, java.io.FileDescriptor fd, long offset, long length) throws java.io.IOException {
    query(); if (sourced) throw new IllegalStateException("MediaPlayer source already set");
    try { peer = nativeSource(path, fd, offset, length); }
    catch (UnsatisfiedLinkError missing) { throw new java.io.IOException("Host audio media backend unavailable", missing); }
    if (peer == 0) throw new java.io.IOException("Unsupported audio media source");
    sourced = true; fresh = false;
    nativeSurface(peer, display != null ? display.getSurface() : surface);
    nativeControl(peer, 5, looping ? 1 : 0); nativeVolume(peer, leftVolume, rightVolume);
  }
  public synchronized void setDataSource(String path) throws java.io.IOException {
    java.util.Objects.requireNonNull(path); source(path, null, 0, Long.MAX_VALUE);
  }
  public synchronized void setDataSource(java.io.FileDescriptor fd) throws java.io.IOException {
    setDataSource(fd, 0, Long.MAX_VALUE);
  }
  public synchronized void setDataSource(java.io.FileDescriptor fd, long offset, long length) throws java.io.IOException {
    java.util.Objects.requireNonNull(fd);
    if (!fd.valid()) throw new java.io.IOException("Invalid media file descriptor");
    if (offset < 0 || length < 0) throw new IllegalArgumentException("Invalid media region");
    source(null, fd, offset, length);
  }
  public synchronized void setDataSource(String path, java.util.Map<String,String> headers) throws java.io.IOException {
    if (headers != null && !headers.isEmpty()) throw new java.io.IOException("Media network protocols unavailable");
    setDataSource(path);
  }
  public synchronized void setDataSource(android.content.Context context, android.net.Uri uri) throws java.io.IOException {
    java.util.Objects.requireNonNull(context); java.util.Objects.requireNonNull(uri);
    if (uri.getScheme() == null || "file".equals(uri.getScheme())) setDataSource(uri.getPath());
    else throw new java.io.IOException("Media URI provider unavailable");
  }
  public static MediaPlayer create(android.content.Context context, android.net.Uri uri) {
    MediaPlayer player = new MediaPlayer();
    try { player.setDataSource(context, uri); player.prepare(); return player; }
    catch (java.io.IOException | RuntimeException failure) { player.release(); return null; }
  }
  public synchronized void reset() {
    live(); generation++; error = false; fresh = false; looping = false;
    if (peer != 0) nativeRelease(peer); peer = 0;
    sourced = prepared = preparing = completionPosted = stopped = false;
    surface = null; display = null; leftVolume = rightVolume = 1;
    if (events != null) events.removeCallbacksAndMessages(null);
  }
  public synchronized void release() {
    if (peer != 0) nativeRelease(peer); peer = 0;
    sourced = prepared = preparing = completionPosted = stopped = false;
    released = true; generation++; surface = null; display = null;
    errorListener = null; completionListener = null; preparedListener = null;
    sizeListener = null; seekListener = null; bufferingListener = null; infoListener = null;
    if (events != null) events.removeCallbacksAndMessages(null);
  }
  public synchronized void setOnErrorListener(OnErrorListener value) { errorListener = value; }
  public synchronized void setOnCompletionListener(OnCompletionListener value) { completionListener = value; }
  public synchronized void setOnPreparedListener(OnPreparedListener value) { preparedListener = value; }
  public synchronized void setOnVideoSizeChangedListener(OnVideoSizeChangedListener value) { sizeListener = value; }
  public synchronized void setOnSeekCompleteListener(OnSeekCompleteListener value) { seekListener = value; }
  public synchronized void setOnBufferingUpdateListener(OnBufferingUpdateListener value) { bufferingListener = value; }
  public synchronized void setOnInfoListener(OnInfoListener value) { infoListener = value; }
  public interface OnBufferingUpdateListener { void onBufferingUpdate(MediaPlayer player, int percent); }
  public interface OnCompletionListener { void onCompletion(MediaPlayer player); }
  public interface OnErrorListener { boolean onError(MediaPlayer player, int what, int extra); }
  public interface OnInfoListener { boolean onInfo(MediaPlayer player, int what, int extra); }
  public interface OnPreparedListener { void onPrepared(MediaPlayer player); }
  public interface OnSeekCompleteListener { void onSeekComplete(MediaPlayer player); }
  public interface OnVideoSizeChangedListener { void onVideoSizeChanged(MediaPlayer player, int width, int height); }
}
