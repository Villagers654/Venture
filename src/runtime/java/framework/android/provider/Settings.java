package android.provider;
public final class Settings {
  public static class SettingNotFoundException extends Exception {
    public SettingNotFoundException(String name) {
      super(name);
    }
  }
  public static class System {
    public static final android.net.Uri CONTENT_URI = android.net.Uri.parse("content://settings/system");
    public static final android.net.Uri DEFAULT_RINGTONE_URI = getUriFor("ringtone"),
        DEFAULT_NOTIFICATION_URI = getUriFor("notification_sound"), DEFAULT_ALARM_ALERT_URI = getUriFor("alarm_alert");
    public static android.net.Uri getUriFor(String name) {
      return android.net.Uri.parse("content://settings/system/" + name);
    }
    public static int getInt(android.content.ContentResolver r, String name, int fallback) {
      return name.equals("accelerometer_rotation") ? 0 : fallback;
    }
    public static int getInt(android.content.ContentResolver r, String name)
        throws SettingNotFoundException {
      int n = getInt(r, name, Integer.MIN_VALUE);
      if (n == Integer.MIN_VALUE)
        throw new SettingNotFoundException(name);
      return n;
    }
  }
  public static class Secure {
    public static final String ANDROID_ID = "android_id";
    public static final android.net.Uri CONTENT_URI = android.net.Uri.parse("content://settings/secure");
    public static String getString(android.content.ContentResolver r, String key) {
      return ANDROID_ID.equals(key) ? androidId() : null;
    }
    public static int getInt(android.content.ContentResolver r, String key, int def) {
      return def;
    }
    private static String androidId;
    private static synchronized String androidId() {
      if (androidId != null) return androidId;
      try {
        var env = com.venture.runtime.RuntimeEnvironment.get();
        java.io.File user = env.data.getParentFile();
        byte[] key = com.venture.runtime.Ssaid.userKey((user != null ? user : env.data).toPath());
        return androidId = com.venture.runtime.Ssaid.derive(key, com.venture.runtime.ApkSignatures.read(env.apk));
      } catch (java.io.IOException | java.security.GeneralSecurityException e) {
        throw new IllegalStateException("Cannot derive ANDROID_ID", e);
      }
    }
  }
  public static class Global {
    public static final String DEVICE_NAME = "device_name";
    public static final android.net.Uri CONTENT_URI = android.net.Uri.parse("content://settings/global");
    public static android.net.Uri getUriFor(String name) { return android.net.Uri.withAppendedPath(CONTENT_URI, name); }
    public static String getString(android.content.ContentResolver r, String key) {
      if (DEVICE_NAME.equals(key)) return android.os.Build.MODEL;
      // The VrApi runtime has no Android developer-mode services enabled.
      // Older OVRPlugin queries this setting without a fallback during VR entry.
      return "vrmode_developer_mode".equals(key) ? "0" : null;
    }
    public static int getInt(android.content.ContentResolver r, String key, int def) {
      try {
        return Integer.parseInt(getString(r, key));
      } catch (NumberFormatException e) {
        return def;
      }
    }
    public static float getFloat(android.content.ContentResolver r, String key, float def) {
      String value = getString(r, key);
      if (value == null) return def;
      try {
        return Float.parseFloat(value);
      } catch (NumberFormatException e) {
        return def;
      }
    }
    // AOSP core/java/android/provider/Settings.java: Global.getInt without a
    // default throws for a missing setting or a value which is not an integer.
    public static int getInt(android.content.ContentResolver r, String key)
        throws SettingNotFoundException {
      try {
        return Integer.parseInt(getString(r, key));
      } catch (NumberFormatException e) {
        throw new SettingNotFoundException(key);
      }
    }
  }
}
