package android.app;

import android.content.Context;
import android.graphics.HardwareRenderer;
import java.io.File;

public final class ActivityThread {
  private static volatile ActivityThread current;
  private ActivityThread() {}

  // AOSP ActivityThread.java: attach publishes the process's ActivityThread;
  // currentActivityThread is process-wide, including JNI calls on audio workers.
  public static synchronized void ventureAttach() {
    if (current == null) current = new ActivityThread();
  }
  public static ActivityThread currentActivityThread() {
    return current;
  }
  public Application getApplication() {
    return currentApplication();
  }

  // AOSP frameworks/base/core/java/android/app/ActivityThread.java: returns
  // the process's initial Application. Avatar2 queries this through JNI.
  public static Application currentApplication() {
    return com.venture.runtime.RuntimeEnvironment.get().application;
  }

  // AOSP handleBindApplication runs setupGraphicsSupport before it makes the
  // Application: the generated graphics code cache lives in the code cache
  // directory of the device-protected storage context (one storage here).
  public void ventureBindApplication(Context appContext) {
    setupGraphicsSupport(appContext);
    // AOSP handleBindApplication (Android 10+): the cache directory is the usable
    // directory for temporary files, as java.io.tmpdir and the TMPDIR environment
    // variable. Without TMPDIR, Mono's Path.GetTempPath is an unwritable default.
    final File cacheDir = appContext.getCacheDir();
    if (cacheDir != null) {
      System.setProperty("java.io.tmpdir", cacheDir.getAbsolutePath());
      try {
        android.system.Os.setenv("TMPDIR",
            com.venture.runtime.RuntimeEnvironment.toGuestPath(cacheDir.getAbsolutePath()), true);
      } catch (android.system.ErrnoException ex) {
        android.util.Log.w("ActivityThread", "Unable to initialize \"TMPDIR\"", ex);
      }
    }
  }
  private void setupGraphicsSupport(Context context) {
    final File codeCacheDir = context.getCodeCacheDir();
    if (codeCacheDir != null) HardwareRenderer.setupDiskCache(codeCacheDir);
  }
}
