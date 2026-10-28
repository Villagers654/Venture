package com.venture.runtime;

/** Host boundary. Tests supply a recording implementation; production uses JNI. */
public interface Host {
  default byte[] readVirtualFile(byte[] path) {
    throw new UnsupportedOperationException("Guest virtual filesystem unavailable");
  }
  default boolean virtualFileExists(byte[] path) {
    throw new UnsupportedOperationException("Guest virtual filesystem unavailable");
  }
  default int setEnvironment(byte[] name, byte[] value, boolean overwrite) {
    throw new UnsupportedOperationException("Guest environment unavailable");
  }
  default String guestErrorString(int error) {
    throw new UnsupportedOperationException("Guest errno descriptions unavailable");
  }
  default boolean hasInternetConnectivity() { return false; }
  default void restartProcess() {
    throw new UnsupportedOperationException("Host process restart unavailable");
  }
  default void restartProcess(byte[] launchIntent) {
    throw new UnsupportedOperationException("Host process restart payload unavailable");
  }
  default byte[] coldLaunchIntent() { return null; }
  default int vulkanHardwareVersion() { return 0; }
  // libEGL egl_set_cache_filename (HardwareRenderer.setupDiskCache). Hosts
  // without guest EGL have no blob cache to persist.
  default void setupShadersDiskCache(String cacheFile) {}
  default int activeNetworkType() {
    throw new UnsupportedOperationException("Host network inventory unavailable");
  }
  default boolean debuggerConnected() {
    throw new UnsupportedOperationException("Host debugger state unavailable");
  }
  default long[] processMemory() {
    throw new UnsupportedOperationException("Host process memory accounting unavailable");
  }
  default byte[] systemProperty(String key) {
    throw new UnsupportedOperationException("Host system property store unavailable");
  }
  default android.media.AudioDeviceInfo[] audioDevices() {
    throw new UnsupportedOperationException("Host audio device inventory unavailable");
  }
  default int[] audioOutputProperties() { return new int[]{0, 0}; }
  default boolean isMusicActive() {
    throw new UnsupportedOperationException("Host audio activity unavailable");
  }
  int guestTid();
  // Pair on the calling thread, before claiming any pending relaunch intent.
  default void beginProcessExit() {}
  default void endProcessExit() {}
  default void exitProcess(int status) {
    throw new UnsupportedOperationException("Host process lifecycle unavailable");
  }
  default void setThreadPriority(int tid, int priority) {
    if (priority < -20 || priority > 19 || (tid != 0 && tid != guestTid()))
      throw new IllegalArgumentException("Invalid priority or unknown thread");
    Thread.currentThread().setPriority(Math.max(1, Math.min(10, 5 - priority / 4)));
  }
  void bindClass(Class<?> cls);
  void log(int priority, String tag, String message);
  void loadLibrary(String name, ClassLoader loader);
  long createSurface(int width, int height, int format);
  void releaseSurface(long handle);
  long createNativeActivity(android.app.NativeActivity activity, String library, String entry);
  void nativeActivityEvent(long handle, int event, long surface);
  byte[] readAsset(String path);
  default java.io.InputStream openAsset(String path, int mode) throws java.io.IOException {
    byte[] data = readAsset(path);
    if (data == null) throw new java.io.FileNotFoundException(path);
    return new java.io.ByteArrayInputStream(data);
  }
  default android.content.res.AssetFileDescriptor openAssetFd(String path) throws java.io.IOException {
    throw new java.io.IOException("Host asset file descriptors unavailable");
  }
  String[] listAssets(String path);
}
