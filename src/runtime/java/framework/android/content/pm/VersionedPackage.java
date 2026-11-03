package android.content.pm;
/** A package name with a version code (API 26); VERSION_CODE_HIGHEST matches any version. */
public final class VersionedPackage implements android.os.Parcelable {
  private final String packageName;
  private final long versionCode;
  public VersionedPackage(String packageName, int versionCode) {
    this.packageName = packageName;
    this.versionCode = versionCode;
  }
  public VersionedPackage(String packageName, long versionCode) {
    this.packageName = packageName;
    this.versionCode = versionCode;
  }
  public String getPackageName() {
    return packageName;
  }
  @Deprecated
  public int getVersionCode() {
    return (int) (versionCode & 0x7fffffff);
  }
  public long getLongVersionCode() {
    return versionCode;
  }
  @Override
  public String toString() {
    return "VersionedPackage[" + packageName + "/" + versionCode + "]";
  }
  @Override
  public boolean equals(Object o) {
    return o instanceof VersionedPackage v && v.versionCode == versionCode
        && java.util.Objects.equals(v.packageName, packageName);
  }
  @Override
  public int hashCode() {
    return java.util.Objects.hash(packageName, versionCode);
  }
  public int describeContents() {
    return 0;
  }
  public void writeToParcel(android.os.Parcel dest, int flags) {
    dest.writeString(packageName);
    dest.writeLong(versionCode);
  }
}
