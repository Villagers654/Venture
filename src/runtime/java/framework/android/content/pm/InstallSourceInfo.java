package android.content.pm;

/** API 30. The installer getInstallerPackageName names (a Quest's store, com.oculus.ocms). */
public final class InstallSourceInfo {
  public static final int PACKAGE_SOURCE_UNSPECIFIED = 0;
  private final String installer;
  // AOSP's hidden constructor (initiating, its signing info, originating, installing).
  public InstallSourceInfo(String initiating, SigningInfo initiatingSigning, String originating, String installing) {
    this.installer = installing;
  }
  public String getInstallingPackageName() { return installer; }
  public String getInitiatingPackageName() { return installer; }
  public String getOriginatingPackageName() { return null; }
  public String getUpdateOwnerPackageName() { return null; }
  public SigningInfo getInitiatingPackageSigningInfo() { return null; }
  public int getPackageSource() { return PACKAGE_SOURCE_UNSPECIFIED; }
}
