package android.os;

import com.venture.runtime.RuntimeEnvironment;
import java.io.File;

/** App-owned emulated storage; Java and native VFS share these backing directories. */
public final class Environment {
  public static final String MEDIA_MOUNTED = "mounted", MEDIA_MOUNTED_READ_ONLY = "mounted_ro",
      MEDIA_UNMOUNTED = "unmounted", MEDIA_REMOVED = "removed";
  public static final String DIRECTORY_MUSIC = "Music", DIRECTORY_PODCASTS = "Podcasts",
      DIRECTORY_RINGTONES = "Ringtones", DIRECTORY_ALARMS = "Alarms",
      DIRECTORY_NOTIFICATIONS = "Notifications", DIRECTORY_PICTURES = "Pictures",
      DIRECTORY_MOVIES = "Movies", DIRECTORY_DOWNLOADS = "Download", DIRECTORY_DCIM = "DCIM",
      DIRECTORY_DOCUMENTS = "Documents";
  private Environment() {}
  // Dedicated host-backed partition roots keep JDK File/StatFs usable while
  // the native JNI path projection exposes the Android partition names.
  public static File getDataDirectory() {
    return RuntimeEnvironment.directory(new File(RuntimeEnvironment.get().data, "volumes/data"));
  }
  public static File getRootDirectory() {
    return RuntimeEnvironment.directory(new File(RuntimeEnvironment.get().data, "volumes/system"));
  }
  public static File getExternalStorageDirectory() {
    var environment = RuntimeEnvironment.get();
    File root = RuntimeEnvironment.directory(new File(environment.data, "external"));
    return root;
  }
  public static File getExternalStoragePublicDirectory(String type) {
    if (type == null || type.contains("/") || type.contains("\\") || type.equals(".."))
      throw new IllegalArgumentException("Invalid public directory");
    return new File(getExternalStorageDirectory(), type);
  }
  public static String getExternalStorageState() {
    return getExternalStorageState(getExternalStorageDirectory());
  }
  public static String getExternalStorageState(File path) {
    var root = getExternalStorageDirectory().toPath().toAbsolutePath().normalize();
    var app = RuntimeEnvironment.get().external.toPath().toAbsolutePath().normalize().getParent();
    var candidate = path.toPath().toAbsolutePath().normalize();
    if (!candidate.startsWith(root) && !candidate.startsWith(app)) return MEDIA_REMOVED;
    return root.toFile().canWrite() ? MEDIA_MOUNTED : MEDIA_MOUNTED_READ_ONLY;
  }
  public static boolean isExternalStorageEmulated() { return true; }
  public static boolean isExternalStorageRemovable() { return false; }
}
