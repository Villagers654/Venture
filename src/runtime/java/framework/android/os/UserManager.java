package android.os;

/** AOSP android.os.UserManager for the single, unlocked owner user of a Quest. */
public class UserManager {
  public static final String DISALLOW_MODIFY_ACCOUNTS = "no_modify_accounts";
  public static final String DISALLOW_CONFIG_WIFI = "no_config_wifi";
  public static final String DISALLOW_INSTALL_APPS = "no_install_apps";
  public static boolean supportsMultipleUsers() { return false; }
  public boolean isUserUnlocked() { return true; }
  public boolean isUserRunning() { return true; }
  public boolean isSystemUser() { return true; }
  public boolean isAdminUser() { return true; }
  public boolean isDemoUser() { return false; }
  public boolean isGuestUser() { return false; }
  public boolean isManagedProfile() { return false; }
  public boolean isQuietModeEnabled() { return false; }
  public boolean isUserAGoat() { return false; }
  public String getUserName() { return ""; }
  public int getUserCount() { return 1; }
  public Bundle getUserRestrictions() { return new Bundle(); }
  public boolean hasUserRestriction(String restriction) { return false; }
  public Bundle getApplicationRestrictions(String packageName) { return new Bundle(); }
  public long getUserCreationTime() { return 0; }
}
