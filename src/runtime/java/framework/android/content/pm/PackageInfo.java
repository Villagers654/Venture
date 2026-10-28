package android.content.pm;
public class PackageInfo {
  public String packageName, versionName;
  public int versionCode;
  public ApplicationInfo applicationInfo;
  public ActivityInfo[] activities;
  public ProviderInfo[] providers;
  public ServiceInfo[] services;
  public String[] requestedPermissions;
  public Signature[] signatures;
  public SigningInfo signingInfo;
  public long firstInstallTime, lastUpdateTime;
  public long getLongVersionCode() {
    return Integer.toUnsignedLong(versionCode);
  }
}
