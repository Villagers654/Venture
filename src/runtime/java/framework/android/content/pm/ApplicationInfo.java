package android.content.pm;
public class ApplicationInfo extends PackageItemInfo {
  public static final int FLAG_SYSTEM = 1, FLAG_DEBUGGABLE = 2;
  public String className, sourceDir, publicSourceDir, dataDir, nativeLibraryDir, processName;
  public String[] splitSourceDirs, splitPublicSourceDirs, sharedLibraryFiles;
  public int targetSdkVersion = 32, minSdkVersion, flags, theme, uid = 10000;
  public boolean enabled = true;
}
