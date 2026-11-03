package android.content.pm;
public class ProviderInfo extends ComponentInfo {
  public String authority;
  public String readPermission;
  public String writePermission;
  public boolean grantUriPermissions;
  public boolean forceUriPermissions;
  public boolean multiprocess;
  public int initOrder;
  public int flags;
  public boolean isSyncable;
}
