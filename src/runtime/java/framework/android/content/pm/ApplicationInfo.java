package android.content.pm;
public class ApplicationInfo extends PackageItemInfo {
  public static final int FLAG_SYSTEM = 1, FLAG_DEBUGGABLE = 2;
  public static final int FLAG_HAS_CODE = 4, FLAG_ALLOW_BACKUP = 1 << 15, FLAG_LARGE_HEAP = 1 << 20,
                          FLAG_SUPPORTS_RTL = 1 << 22, FLAG_HARDWARE_ACCELERATED = 1 << 29;
  public String className, sourceDir, publicSourceDir, dataDir, nativeLibraryDir, processName;
  public String[] splitSourceDirs, splitPublicSourceDirs, sharedLibraryFiles;
  public int targetSdkVersion = 32, minSdkVersion, flags, theme, uid = 10000;
  public boolean enabled = true;
}
