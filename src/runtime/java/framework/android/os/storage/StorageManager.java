package android.os.storage;

import android.os.Handler;
import android.os.Looper;
import com.venture.runtime.ObbImage;
import com.venture.runtime.RuntimeEnvironment;
import java.io.File;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.ConcurrentHashMap;

/**
 * AOSP android.os.storage.StorageManager, OBB part. Android's vold mounts an OBB filesystem image
 * (a FAT volume with the OBB footer). Here an unencrypted image's files are unpacked once into
 * the app's cache (com.venture.runtime.ObbImage) and that directory is the mount: its guest path
 * is getMountedObbPath. Quest titles that ship their OBBs as Unity/zip archives read them
 * directly; mounting one, or an encrypted image, ends as on Android for a file it cannot mount:
 * the listener hears ERROR_COULD_NOT_MOUNT and nothing is mounted.
 */
public class StorageManager {
  private static final Handler MAIN = new Handler(Looper.getMainLooper());
  // rawPath -> the mount's guest path, for every StorageManager of the process (vold's table).
  private static final Map<String, String> MOUNTED = new ConcurrentHashMap<>();

  public boolean mountObb(String rawPath, String key, OnObbStateChangeListener listener) {
    Objects.requireNonNull(rawPath, "rawPath cannot be null");
    Objects.requireNonNull(listener, "listener cannot be null");
    File file = new File(RuntimeEnvironment.toHostPath(rawPath));
    if (!file.isFile()) throw new IllegalArgumentException("Failed to resolve path: " + rawPath);
    int state;
    if (MOUNTED.containsKey(rawPath)) {
      state = OnObbStateChangeListener.ERROR_ALREADY_MOUNTED;
    } else if (key != null && !key.isEmpty()) {
      state = OnObbStateChangeListener.ERROR_COULD_NOT_MOUNT;
    } else {
      state = OnObbStateChangeListener.ERROR_COULD_NOT_MOUNT;
      try {
        RuntimeEnvironment env = RuntimeEnvironment.current();
        File into = env == null ? null : new File(env.cache, "obb-mounts/" + RuntimeEnvironment.hostName(file.getName()));
        File mount = into == null ? null : ObbImage.unpack(file, into);
        if (mount != null) {
          MOUNTED.put(rawPath, RuntimeEnvironment.toGuestPath(mount.getPath()));
          state = OnObbStateChangeListener.MOUNTED;
        }
      } catch (java.io.IOException | RuntimeException unreadable) {
        android.util.Log.w("StorageManager", "OBB image " + file.getName() + " cannot be unpacked: " + unreadable);
        state = OnObbStateChangeListener.ERROR_INTERNAL;
      }
    }
    if (state != OnObbStateChangeListener.MOUNTED)
      android.util.Log.w("StorageManager", "OBB mount of " + rawPath + " ends with state " + state);
    final int result = state;
    MAIN.post(() -> listener.onObbStateChange(rawPath, result));
    return true;
  }
  public boolean unmountObb(String rawPath, boolean force, OnObbStateChangeListener listener) {
    Objects.requireNonNull(rawPath, "rawPath cannot be null");
    Objects.requireNonNull(listener, "listener cannot be null");
    final int state = MOUNTED.remove(rawPath) != null ? OnObbStateChangeListener.UNMOUNTED
                                                       : OnObbStateChangeListener.ERROR_NOT_MOUNTED;
    MAIN.post(() -> listener.onObbStateChange(rawPath, state));
    return true;
  }
  public boolean isObbMounted(String rawPath) {
    Objects.requireNonNull(rawPath, "rawPath cannot be null");
    return MOUNTED.containsKey(rawPath);
  }
  public String getMountedObbPath(String rawPath) {
    Objects.requireNonNull(rawPath, "rawPath cannot be null");
    return MOUNTED.get(rawPath);
  }
}
