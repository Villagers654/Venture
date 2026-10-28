package android.content;
public interface ServiceConnection {
  void onServiceConnected(ComponentName name, android.os.IBinder service);
  void onServiceDisconnected(ComponentName name);
  default void onBindingDied(ComponentName name) {}
  default void onNullBinding(ComponentName name) {}
}
