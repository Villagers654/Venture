package android.os.storage;

/** AOSP android.os.storage.OnObbStateChangeListener. */
public abstract class OnObbStateChangeListener {
  public static final int MOUNTED = 1, UNMOUNTED = 2, ERROR_INTERNAL = 20, ERROR_COULD_NOT_MOUNT = 21,
      ERROR_COULD_NOT_UNMOUNT = 22, ERROR_NOT_MOUNTED = 23, ERROR_ALREADY_MOUNTED = 24,
      ERROR_PERMISSION_DENIED = 25;

  public void onObbStateChange(String path, int state) {}
}
