package android.app;
public class Application extends android.content.ContextWrapper {
  public static String getProcessName() {
    var environment = com.venture.runtime.RuntimeEnvironment.get();
    var name = environment.manifest.info.applicationInfo.processName;
    return name == null ? environment.manifest.info.packageName : name;
  }
  public Application() {
    super(null);
  }
  public final void ventureAttach(android.content.Context context) {
    attachBaseContext(new com.venture.runtime.ComponentContext(context, this));
  }
  public void onCreate() {}
  public void onTerminate() {}
  // AOSP Application dispatches a snapshot so callbacks can unregister while
  // receiving configuration and memory notifications.
  public void onLowMemory() {
    for (var callback : componentCallbacks) callback.onLowMemory();
  }
  public void onTrimMemory(int level) {
    for (var callback : componentCallbacks)
      if (callback instanceof android.content.ComponentCallbacks2 extended) extended.onTrimMemory(level);
  }
  public void onConfigurationChanged(android.content.res.Configuration c) {
    for (var callback : componentCallbacks) callback.onConfigurationChanged(c);
  }
  public interface ActivityLifecycleCallbacks {
    default void onActivityPreCreated(Activity a, android.os.Bundle b) {}
    default void onActivityPostCreated(Activity a, android.os.Bundle b) {}
    default void onActivityPreStarted(Activity a) {}
    default void onActivityPostStarted(Activity a) {}
    default void onActivityPreResumed(Activity a) {}
    default void onActivityPostResumed(Activity a) {}
    default void onActivityPrePaused(Activity a) {}
    default void onActivityPostPaused(Activity a) {}
    default void onActivityPreStopped(Activity a) {}
    default void onActivityPostStopped(Activity a) {}
    default void onActivityPreSaveInstanceState(Activity a, android.os.Bundle b) {}
    default void onActivityPostSaveInstanceState(Activity a, android.os.Bundle b) {}
    default void onActivityPreDestroyed(Activity a) {}
    default void onActivityPostDestroyed(Activity a) {}
    void onActivityCreated(Activity a, android.os.Bundle b);
    void onActivityStarted(Activity a);
    void onActivityResumed(Activity a);
    void onActivityPaused(Activity a);
    void onActivityStopped(Activity a);
    void onActivitySaveInstanceState(Activity a, android.os.Bundle b);
    void onActivityDestroyed(Activity a);
  }
  private final java.util.List<ActivityLifecycleCallbacks> callbacks =
      new java.util.concurrent.CopyOnWriteArrayList<>();
  public void registerActivityLifecycleCallbacks(ActivityLifecycleCallbacks c) {
    callbacks.add(c);
  }
  public void unregisterActivityLifecycleCallbacks(ActivityLifecycleCallbacks c) {
    callbacks.remove(c);
  }
  public java.util.List<ActivityLifecycleCallbacks> lifecycleCallbacks() {
    return java.util.List.copyOf(callbacks);
  }
}
