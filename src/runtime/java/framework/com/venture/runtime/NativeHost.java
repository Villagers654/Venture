package com.venture.runtime;

public final class NativeHost implements Host {
  public native byte[] readVirtualFile(byte[] path);
  public native boolean virtualFileExists(byte[] path);
  public native int setEnvironment(byte[] name, byte[] value, boolean overwrite);
  public native String guestErrorString(int error);
  public native void restartProcess();
  public native void restartProcess(byte[] launchIntent);
  public native byte[] coldLaunchIntent();
  public native int vulkanHardwareVersion();
  public native void setupShadersDiskCache(String cacheFile);
  public native int activeNetworkType();
  public native boolean hasInternetConnectivity();
  public native long[] processMemory();
  public native boolean debuggerConnected();
  public native byte[] systemProperty(String key);
  public native android.media.AudioDeviceInfo[] audioDevices();
  public native boolean isMusicActive();
  public native int[] audioOutputProperties();
  private static boolean nativeLoopersAvailable;
  public static boolean hasNativeLoopers() { return nativeLoopersAvailable; }
  public static native long prepareLooper();
  public static native void pollLooper(long handle, int timeoutMillis);
  public static native void wakeLooper(long handle);
  public native int guestTid();
  public native void beginProcessExit();
  public native void endProcessExit();
  public native void exitProcess(int status);
  public native void setThreadPriority(int tid, int priority);
  public void bindClass(Class<?> cls) {
    NativeLoader.bindClass(cls);
  }
  public native void log(int priority, String tag, String message);
  public native void loadLibrary(String name, ClassLoader loader);
  public native long createSurface(int width, int height, int format);
  public native void releaseSurface(long handle);
  public static native int copySurfacePixels(long handle, int left, int top, int right, int bottom,
      int width, int height, int[] argb);
  public native long createNativeActivity(
      android.app.NativeActivity activity, String library, String entry);
  public native void nativeActivityEvent(long handle, int event, long surface);
  public native byte[] readAsset(String path);
  private static native long openAssetStream(String path) throws java.io.IOException;
  private static native long assetStreamLength(long handle);
  private static native int readAssetStream(long handle, byte[] data, int offset, int count) throws java.io.IOException;
  private static native long seekAssetStream(long handle, long offset) throws java.io.IOException;
  private static native void closeAssetStream(long handle);
  public java.io.InputStream openAsset(String path, int mode) throws java.io.IOException {
    long handle = openAssetStream(path);
    PackageInputStream.State state;
    try { state = new PackageInputStream.State(handle); }
    catch (RuntimeException | Error failure) { closeAssetStream(handle); throw failure; }
    try {
      return new PackageInputStream(state, assetStreamLength(handle));
    } catch (RuntimeException | Error failure) {
      state.run();
      throw failure;
    }
  }

  // PackageAssets already owns ZIP streaming and CRC validation. Java only owns
  // its lifetime and InputStream cursor; the Cleaner is the abandoned-stream fallback.
  private static final class PackageInputStream extends java.io.InputStream {
    private static final java.lang.ref.Cleaner CLEANER = java.lang.ref.Cleaner.create();
    private static final class State implements Runnable {
      long handle;
      State(long handle) { this.handle = handle; }
      void ensureOpen() { if (handle == 0) throw new IllegalStateException("Asset is closed"); }
      public synchronized void run() {
        if (handle != 0) { closeAssetStream(handle); handle = 0; }
      }
    }
    private final State state;
    private final java.lang.ref.Cleaner.Cleanable cleanup;
    private final long length;
    private long position, mark;
    private byte[] single;
    PackageInputStream(State state, long length) {
      this.state = state;
      this.length = length;
      cleanup = CLEANER.register(this, state);
    }
    public int read() throws java.io.IOException {
      synchronized (state) {
        state.ensureOpen();
        if (single == null) single = new byte[1];
        return read(single, 0, 1) < 0 ? -1 : single[0] & 255;
      }
    }
    public int read(byte[] data, int offset, int count) throws java.io.IOException {
      synchronized (state) {
        state.ensureOpen();
        java.util.Objects.checkFromIndexSize(offset, count, data.length);
        if (count == 0) return 0;
        int result = readAssetStream(state.handle, data, offset, count);
        if (result > 0) position += result;
        return result;
      }
    }
    public long skip(long count) throws java.io.IOException {
      synchronized (state) {
        state.ensureOpen();
        if (count <= 0) return 0;
        long next = position + Math.min(count, length - position);
        long previous = position;
        position = seekAssetStream(state.handle, next);
        return position - previous;
      }
    }
    public int available() {
      synchronized (state) {
        state.ensureOpen();
        return (int)Math.min(Integer.MAX_VALUE, length - position);
      }
    }
    public boolean markSupported() { return true; }
    public void mark(int ignoredReadLimit) {
      synchronized (state) { state.ensureOpen(); mark = position; }
    }
    public void reset() throws java.io.IOException {
      synchronized (state) { state.ensureOpen(); position = seekAssetStream(state.handle, mark); }
    }
    public void close() { cleanup.clean(); }
  }
  public native android.content.res.AssetFileDescriptor openAssetFd(String path) throws java.io.IOException;
  private static android.content.res.AssetFileDescriptor openAssetRegion(String path, long offset, long length)
      throws java.io.IOException {
    var descriptor = android.os.ParcelFileDescriptor.open(new java.io.File(path),
        android.os.ParcelFileDescriptor.MODE_READ_ONLY);
    try {
      return new android.content.res.AssetFileDescriptor(descriptor, offset, length);
    } catch (RuntimeException | Error failure) {
      try { descriptor.close(); } catch (java.io.IOException cleanup) { failure.addSuppressed(cleanup); }
      throw failure;
    }
  }
  public native String[] listAssets(String path);
}
