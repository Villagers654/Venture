package android.app;

import android.content.*;
import android.os.IBinder;

/**
 * Service lifecycle for the hosted application's local services.
 */
public abstract class Service extends ContextWrapper {
  public static final int START_STICKY_COMPATIBILITY = 0, START_STICKY = 1, START_NOT_STICKY = 2,
                          START_REDELIVER_INTENT = 3, START_FLAG_REDELIVERY = 1,
                          START_FLAG_RETRY = 2;
  public Service() {
    super(null);
  }
  public final void ventureAttach(Context context) { attachBaseContext(new com.venture.runtime.ComponentContext(context, this)); }
  public final void stopSelf() { com.venture.runtime.LocalServices.stopSelf(this, -1); }
  public final void stopSelf(int startId) { com.venture.runtime.LocalServices.stopSelf(this, startId); }
  public Application getApplication() {
    return com.venture.runtime.RuntimeEnvironment.get().application;
  }
  public void onCreate() {}
  public void onDestroy() {}
  public void onStart(Intent intent, int startId) {}
  public int onStartCommand(Intent intent, int flags, int startId) {
    onStart(intent, startId);
    return START_STICKY_COMPATIBILITY;
  }
  public abstract IBinder onBind(Intent intent);
  public boolean onUnbind(Intent intent) {
    return false;
  }
  public void onRebind(Intent intent) {}
  public void onTaskRemoved(Intent rootIntent) {}
  public void onConfigurationChanged(android.content.res.Configuration configuration) {}
  public void onLowMemory() {}
  public void onTrimMemory(int level) {}
  // A foreground service posts its ongoing notification; the process is never killed in the background.
  public static final int STOP_FOREGROUND_REMOVE = 1, STOP_FOREGROUND_DETACH = 2;
  private int foregroundId;
  public final void startForeground(int id, Notification notification) {
    if (id == 0) throw new IllegalArgumentException("startForeground id must not be 0");
    if (foregroundId != 0 && foregroundId != id) stopForeground(STOP_FOREGROUND_REMOVE);
    notification.flags |= Notification.FLAG_FOREGROUND_SERVICE;
    ((NotificationManager) getSystemService(NOTIFICATION_SERVICE)).notify(id, notification);
    foregroundId = id;
  }
  public final void startForeground(int id, Notification notification, int foregroundServiceType) {
    startForeground(id, notification);
  }
  @Deprecated public final void stopForeground(boolean removeNotification) {
    stopForeground(removeNotification ? STOP_FOREGROUND_REMOVE : STOP_FOREGROUND_DETACH);
  }
  public final void stopForeground(int flags) {
    if (foregroundId != 0 && (flags & STOP_FOREGROUND_REMOVE) != 0)
      ((NotificationManager) getSystemService(NOTIFICATION_SERVICE)).cancel(foregroundId);
    foregroundId = 0;
  }
  public final int getForegroundServiceType() { return 0; }
}
