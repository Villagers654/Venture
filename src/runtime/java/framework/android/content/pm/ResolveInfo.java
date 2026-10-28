package android.content.pm;

/** Installed manifest activity matched by PackageManager. */
public class ResolveInfo {
  public ActivityInfo activityInfo;
  public ServiceInfo serviceInfo;
  public int priority;
  public int match;
  public boolean isDefault;
}
