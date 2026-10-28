package android.content;
public class ContextWrapper extends Context {
  private Context base;
  public ContextWrapper(Context context) {
    base = context;
  }
  protected void attachBaseContext(Context context) {
    if (base != null)
      throw new IllegalStateException("Base context already set");
    base = context;
  }
  @Override public Context ventureOuterContext() {
    return base == null ? this : base.ventureOuterContext();
  }
  public Context getBaseContext() {
    return base;
  }
  public int checkPermission(String permission, int pid, int uid) {
    return base == null ? super.checkPermission(permission, pid, uid)
                        : base.checkPermission(permission, pid, uid);
  }
  public Context createConfigurationContext(android.content.res.Configuration override) {
    return base == null ? super.createConfigurationContext(override)
                        : base.createConfigurationContext(override);
  }
  public android.view.Display getDisplay() {
    return base == null ? super.getDisplay() : base.getDisplay();
  }
  public android.content.res.Resources getResources() {
    return base == null ? super.getResources() : base.getResources();
  }
  @Override public ComponentName startService(Intent intent) {
    return base == null ? super.startService(intent) : base.startService(intent);
  }
  @Override public boolean stopService(Intent intent) {
    return base == null ? super.stopService(intent) : base.stopService(intent);
  }
  @Override public boolean bindService(Intent intent, ServiceConnection connection, int flags) {
    return base == null ? super.bindService(intent, connection, flags) : base.bindService(intent, connection, flags);
  }
  @Override public boolean bindService(Intent intent, int flags, java.util.concurrent.Executor executor, ServiceConnection connection) {
    return base == null ? super.bindService(intent, flags, executor, connection) : base.bindService(intent, flags, executor, connection);
  }
  @Override public void unbindService(ServiceConnection connection) {
    if (base == null) super.unbindService(connection); else base.unbindService(connection);
  }
  @Override public Intent registerReceiver(BroadcastReceiver receiver, IntentFilter filter) {
    return base == null ? super.registerReceiver(receiver, filter) : base.registerReceiver(receiver, filter);
  }
  @Override public Intent registerReceiver(BroadcastReceiver receiver, IntentFilter filter, int flags) {
    return base == null ? super.registerReceiver(receiver, filter, flags) : base.registerReceiver(receiver, filter, flags);
  }
  @Override public Intent registerReceiver(BroadcastReceiver receiver, IntentFilter filter, String permission, android.os.Handler scheduler) {
    return base == null ? super.registerReceiver(receiver, filter, permission, scheduler)
                        : base.registerReceiver(receiver, filter, permission, scheduler);
  }
  @Override public Intent registerReceiver(BroadcastReceiver receiver, IntentFilter filter, String permission, android.os.Handler scheduler, int flags) {
    return base == null ? super.registerReceiver(receiver, filter, permission, scheduler, flags)
                        : base.registerReceiver(receiver, filter, permission, scheduler, flags);
  }
  @Override public void unregisterReceiver(BroadcastReceiver receiver) {
    if (base == null) super.unregisterReceiver(receiver); else base.unregisterReceiver(receiver);
  }
}
