package android.content.pm;
public class ComponentInfo extends PackageItemInfo {
  public ApplicationInfo applicationInfo;
  public String processName, splitName;
  public boolean enabled = true, exported, directBootAware;
  public int descriptionRes;
  // As AOSP: the component and its application must both be enabled (Play
  // Core's AssetPackManagerFactory checks this at start).
  public boolean isEnabled() {
    return enabled && (applicationInfo == null || applicationInfo.enabled);
  }
  public final int getIconResource() {
    return icon != 0 ? icon : applicationInfo == null ? 0 : applicationInfo.icon;
  }
}
