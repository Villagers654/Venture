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
  // GeckoView exports the serial number to Gecko's environment at start (Vuplex 3D WebView).
  public long getSerialNumberForUser(UserHandle user) { return user != null && user.isSystem() ? 0 : -1; }
  public UserHandle getUserForSerialNumber(long serialNumber) { return serialNumber == 0 ? UserHandle.SYSTEM : null; }
  public java.util.List<UserHandle> getUserProfiles() { return java.util.List.of(UserHandle.SYSTEM); }
}
