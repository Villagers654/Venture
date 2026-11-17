package android.content.pm;
public class PackageInfo {
  public String packageName, versionName;
  public int versionCode;
  public ApplicationInfo applicationInfo;
  public ActivityInfo[] activities;
  public ProviderInfo[] providers;
  public ServiceInfo[] services;
  public static final int REQUESTED_PERMISSION_REQUIRED = 1, REQUESTED_PERMISSION_GRANTED = 2;
  public ActivityInfo[] receivers;
  public String[] requestedPermissions;
  public int[] requestedPermissionsFlags;
  public Signature[] signatures;
  public SigningInfo signingInfo;
  public long firstInstallTime, lastUpdateTime;
  public long getLongVersionCode() {
    return Integer.toUnsignedLong(versionCode);
  }
}
