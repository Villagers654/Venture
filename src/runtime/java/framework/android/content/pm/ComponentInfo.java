package android.content.pm;
public class ComponentInfo extends PackageItemInfo {
  public ApplicationInfo applicationInfo;
  public String processName;
  public boolean enabled = true, exported;
}
