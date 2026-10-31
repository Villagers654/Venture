package android.content.pm;
public class PermissionInfo extends PackageItemInfo {
  public static final int PROTECTION_NORMAL = 0, PROTECTION_DANGEROUS = 1, PROTECTION_SIGNATURE = 2,
                          PROTECTION_MASK_BASE = 0xf, PROTECTION_MASK_FLAGS = 0xfff0;
  public int protectionLevel, flags;
  public String group;
  public PermissionInfo() {}
  public PermissionInfo(PermissionInfo other) {
    name = other.name; packageName = other.packageName; nonLocalizedLabel = other.nonLocalizedLabel;
    protectionLevel = other.protectionLevel; flags = other.flags; group = other.group;
  }
  public int getProtection() { return protectionLevel & PROTECTION_MASK_BASE; }
  public int getProtectionFlags() { return protectionLevel & ~PROTECTION_MASK_BASE; }
}
