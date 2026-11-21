package android.content.pm;
public class ServiceInfo extends ComponentInfo {
  public String permission;
  public int flags;
  public int mForegroundServiceType;  // AOSP's (hidden) field name
  public int getForegroundServiceType() {
    return mForegroundServiceType;
  }
}
