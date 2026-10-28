package android.content.pm;
public class ServiceInfo extends ComponentInfo {
  public String permission;
  public int flags;
  public int foregroundServiceType;
  public int getForegroundServiceType() {
    return foregroundServiceType;
  }
}
