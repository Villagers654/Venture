package android.provider;
public final class Settings {
  public static class SettingNotFoundException extends Exception {
    public SettingNotFoundException(String name) {
      super(name);
    }
  }
  public static class System {
    public static String getStringForUser(android.content.ContentResolver r, String name, int user) {
      return getString(r, name);
    }
    public static final android.net.Uri CONTENT_URI = android.net.Uri.parse("content://settings/system");
    public static final android.net.Uri DEFAULT_RINGTONE_URI = getUriFor("ringtone"),
        DEFAULT_NOTIFICATION_URI = getUriFor("notification_sound"), DEFAULT_ALARM_ALERT_URI = getUriFor("alarm_alert");
    public static android.net.Uri getUriFor(String name) {
      return android.net.Uri.parse("content://settings/system/" + name);
    }
    public static int getInt(android.content.ContentResolver r, String name, int fallback) {
      return name.equals("accelerometer_rotation") ? 0 : fallback;
    }
    public static String getString(android.content.ContentResolver r, String name) {
      return name.equals("accelerometer_rotation") ? "0" : null;
    }
    // font_scale and the other float settings keep Android's defaults: the caller's.
    public static float getFloat(android.content.ContentResolver r, String name, float fallback) {
      return fallback;
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
    public static android.net.Uri getUriFor(String name) {
      return android.net.Uri.withAppendedPath(CONTENT_URI, name);
    }
    public static String getStringForUser(android.content.ContentResolver r, String key, int user) {
      return getString(r, key);
    }
    public static int getIntForUser(android.content.ContentResolver r, String key, int def, int user) {
      return getInt(r, key, def);
    }
    public static float getFloatForUser(android.content.ContentResolver r, String key, float def, int user) {
      return def;
    }
    public static boolean putInt(android.content.ContentResolver r, String key, int value) {
      return false;
    }
    public static boolean putIntForUser(android.content.ContentResolver r, String key, int value, int user) {
      return false;
    }
    public static boolean putString(android.content.ContentResolver r, String key, String value) {
      return false;
    }
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
    public static long getLong(android.content.ContentResolver r, String key, long def) {
      return def;
    }
    public static float getFloat(android.content.ContentResolver r, String key) throws SettingNotFoundException {
      float value = getFloat(r, key, Float.NaN);
      if (Float.isNaN(value)) throw new SettingNotFoundException(key);
      return value;
    }
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
