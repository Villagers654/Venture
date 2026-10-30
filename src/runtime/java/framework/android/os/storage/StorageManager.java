package android.os.storage;

import android.os.Handler;
import android.os.Looper;
import java.io.File;
import java.util.Objects;

/**
 * AOSP android.os.storage.StorageManager, OBB part. Android mounts an OBB only when it is an
 * OBB filesystem image (vold). Quest titles ship their OBBs as Unity/zip archives that apps read
 * directly, and no image filesystem is mounted here, so a mount request ends as Android's does
 * for a file it cannot mount: the listener hears ERROR_COULD_NOT_MOUNT and nothing is mounted.
 */
public class StorageManager {
  private static final Handler MAIN = new Handler(Looper.getMainLooper());

  public boolean mountObb(String rawPath, String key, OnObbStateChangeListener listener) {
    Objects.requireNonNull(rawPath, "rawPath cannot be null");
    Objects.requireNonNull(listener, "listener cannot be null");
    File file = new File(rawPath);
    if (!file.isFile()) throw new IllegalArgumentException("Failed to resolve path: " + rawPath);
    String path = file.getAbsolutePath();
    MAIN.post(() -> listener.onObbStateChange(path, OnObbStateChangeListener.ERROR_COULD_NOT_MOUNT));
    return true;
  }
  public boolean unmountObb(String rawPath, boolean force, OnObbStateChangeListener listener) {
    Objects.requireNonNull(rawPath, "rawPath cannot be null");
    Objects.requireNonNull(listener, "listener cannot be null");
    MAIN.post(() -> listener.onObbStateChange(rawPath, OnObbStateChangeListener.ERROR_NOT_MOUNTED));
    return true;
  }
  public boolean isObbMounted(String rawPath) {
    Objects.requireNonNull(rawPath, "rawPath cannot be null");
    return false;
  }
  public String getMountedObbPath(String rawPath) {
    Objects.requireNonNull(rawPath, "rawPath cannot be null");
    return null;
  }
}
