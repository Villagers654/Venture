package android.app.admin;

/** No device admin; storage is file-based encrypted per user, as on a Quest. */
public class DevicePolicyManager {
  public static final int ENCRYPTION_STATUS_UNSUPPORTED = 0, ENCRYPTION_STATUS_INACTIVE = 1,
                          ENCRYPTION_STATUS_ACTIVATING = 2, ENCRYPTION_STATUS_ACTIVE = 3,
                          ENCRYPTION_STATUS_ACTIVE_DEFAULT_KEY = 4, ENCRYPTION_STATUS_ACTIVE_PER_USER = 5;
  public int getStorageEncryptionStatus() { return ENCRYPTION_STATUS_ACTIVE_PER_USER; }
  public boolean isAdminActive(android.content.ComponentName admin) { return false; }
  public java.util.List<android.content.ComponentName> getActiveAdmins() { return null; }
  public boolean isDeviceOwnerApp(String packageName) { return false; }
  public boolean isProfileOwnerApp(String packageName) { return false; }
}
