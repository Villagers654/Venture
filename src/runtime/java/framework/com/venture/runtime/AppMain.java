package com.venture.runtime;
import android.app.*;
import android.content.*;
import android.os.*;
public final class AppMain {
  // Android main-thread confined; there is one in-process activity task.
  private static final java.util.ArrayDeque<Activity> activities = new java.util.ArrayDeque<>();
  public static final boolean MENU_QUIT = !"0".equals(System.getenv("VENTURE_MENU_QUIT"));
  public static final boolean QUIT_DIAGNOSTICS = "1".equals(System.getenv("VENTURE_MENU_QUIT")) || "1".equals(System.getenv("VENTURE_SHUTDOWN_SNAPSHOTS"));
  public static void quitLog(String message) {
    if (QUIT_DIAGNOSTICS) RuntimeEnvironment.host().log(4, "Lifecycle", message);
  }
  private static volatile boolean shutdownRequested, taskRemoval;
  private static volatile int failureStatus;
  private static volatile int quitNativeCount;
  public static int quitNativeCount() { return quitNativeCount; }
  private static boolean nativeGlue(Activity activity) {
    if (activity instanceof android.app.NativeActivity) return true;
    for (Class<?> type = activity.getClass(); type != null; type = type.getSuperclass())
      if (type.getName().equals("com.google.androidgamesdk.GameActivity")) return true;
    return false;
  }
  private static final java.util.concurrent.ConcurrentHashMap<Activity, Integer> quitStages =
      new java.util.concurrent.ConcurrentHashMap<>();
  // Bits: pause/stop returned, preferences barrier passed, final callback
  // invoked (onDestroy, or onStop when a menu quit removes the task).
  public static void quitStage(Activity activity, int stage) {
    if (!MENU_QUIT) return;
    quitStages.computeIfPresent(activity, (key, previous) -> previous | stage);
  }
  public static int quitProgress() {
    if (!MENU_QUIT || quitStages.isEmpty()) return 0;
    int common = 7;
    for (int stage : quitStages.values()) common &= stage;
    return common | (XmlPreferences.writesQuiet() ? 8 : 0);
  }
  private static final java.util.concurrent.atomic.AtomicReference<Intent> pendingRelaunch =
      new java.util.concurrent.atomic.AtomicReference<>();
  public static boolean takePendingRelaunch() {
    var intent = takeRelaunchIntent();
    if (intent == null) return false;
    if (!intent.getExtras().isEmpty() || !isLauncher(intent))
      throw new UnsupportedOperationException("Cold intent extras or activity require the payload transport");
    return true;
  }
  private static Intent launcherIntent() {
    return RuntimeEnvironment.get().packageManager.getLaunchIntentForPackage(
        RuntimeEnvironment.get().manifest.info.packageName);
  }
  private static boolean isLauncher(Intent intent) {
    var launcher = launcherIntent();
    return launcher != null && launcher.getComponent().equals(intent.getComponent())
        && java.util.Objects.equals(launcher.getAction(), intent.getAction())
        && java.util.Objects.equals(launcher.getCategories(), intent.getCategories());
  }
  /**
   * A new task's intent: the launch activity as the launcher starts it, or
   * another activity the manifest declares, named by its class with at most an
   * action (a 2D menu starting its VR activity, which Android runs in a new
   * task and often in its own process). Data, types and categories have no
   * transport.
   */
  private static Intent takeRelaunchIntent() {
    var intent = pendingRelaunch.getAndSet(null);
    if (intent == null) return null;
    boolean declared = intent.getComponent() != null && RuntimeEnvironment.get().manifest.activities.stream()
        .anyMatch(a -> a.name.equals(intent.getComponent().getClassName()));
    if (intent.getData() != null || intent.getType() != null || !(isLauncher(intent)
        || (declared && (intent.getCategories() == null || intent.getCategories().isEmpty()))))
      throw new UnsupportedOperationException("Cold process restart with intent data, categories or an undeclared activity unavailable");
    return intent;
  }
  public static byte[] takePendingRelaunchPayload() {
    var intent = takeRelaunchIntent();
    if (intent == null) return null;
    // This is our private supervisor wire format, not Android Parcel. Preserve
    // flags and string extras (UE4 RestartExtra); fail before dropping any other
    // Bundle type. No intent contents appear in command lines or diagnostics.
    try {
      var bytes = new java.io.ByteArrayOutputStream();
      var output = new java.io.DataOutputStream(bytes);
      output.writeInt(intent.getFlags());
      // src/runtime/core/launch_payload.h reads the activity to choose the
      // restarted process's framework; empty names the launch activity.
      boolean launcher = isLauncher(intent);
      output.writeUTF(launcher ? "" : intent.getComponent().getClassName());
      output.writeUTF(launcher || intent.getAction() == null ? "" : intent.getAction());
      var extras = intent.getExtras();
      output.writeInt(extras.size());
      for (var key : extras.keySet()) {
        var value = extras.get(key);
        if (key == null || (value != null && !(value instanceof String)))
          throw new UnsupportedOperationException("Cold intent supports only named string extras");
        output.writeUTF(key);
        output.writeBoolean(value != null);
        if (value != null) output.writeUTF((String) value);
      }
      if (bytes.size() > 4095) throw new UnsupportedOperationException("Cold intent exceeds transport capacity");
      return bytes.toByteArray();
    } catch (java.io.IOException failure) {
      throw new IllegalStateException("Cannot encode cold launch intent");
    }
  }
  public static Intent restoreRelaunchIntent(Intent launcher, byte[] payload) {
    if (payload == null || payload.length == 0) return launcher;
    try {
      if (payload.length > 4095) throw new java.io.IOException();
      var input = new java.io.DataInputStream(new java.io.ByteArrayInputStream(payload));
      // The fresh process is the cleared task, so the restart consumed
      // CLEAR_TASK. Keeping it would make an activity that forwards
      // getIntent() restart the process again.
      int flags = input.readInt() & ~Intent.FLAG_ACTIVITY_CLEAR_TASK;
      String activity = input.readUTF(), action = input.readUTF();
      var intent = activity.isEmpty() ? new Intent(launcher)
          : new Intent(action.isEmpty() ? null : action).setComponent(
                new ComponentName(launcher.getComponent().getPackageName(), activity));
      intent.setFlags(flags);
      int count = input.readInt();
      if (count < 0 || count > payload.length / 3) throw new java.io.IOException();
      for (int i = 0; i < count; ++i) {
        String key = input.readUTF();
        intent.putExtra(key, input.readBoolean() ? input.readUTF() : null);
      }
      if (input.available() != 0) throw new java.io.IOException();
      return intent;
    } catch (java.io.IOException failure) {
      throw new IllegalStateException("Invalid cold launch intent");
    }
  }
  public static Intent initialLaunchIntent(Intent launcher, byte[] restartPayload, String extrasFile)
      throws java.io.IOException {
    // The app's restart intent wins. Reapplying initial developer extras could
    // send an otherwise normal restart back to a diagnostic scene.
    if (restartPayload != null && restartPayload.length != 0)
      return restoreRelaunchIntent(launcher, restartPayload);
    if (extrasFile == null) return launcher;
    var extras = new java.util.Properties();
    try (var reader = java.nio.file.Files.newBufferedReader(java.nio.file.Path.of(extrasFile))) {
      extras.load(reader);
    }
    var intent = new Intent(launcher);
    for (String key : extras.stringPropertyNames()) {
      if (key.isEmpty()) throw new IllegalArgumentException("Empty launch extra key");
      intent.putExtra(key, extras.getProperty(key));
    }
    return intent;
  }
  public static void discardPendingRelaunch() { pendingRelaunch.set(null); }
  public static int exitStatus() { return failureStatus; }
  private static boolean isThreadExit(Throwable failure) {
    // Unity's uncaught-exception handler wraps native exits in a diagnostic
    // Error before delegating to ours. Preserve the termination signal.
    var seen = java.util.Collections.newSetFromMap(new java.util.IdentityHashMap<Throwable, Boolean>());
    for (var cause = failure; cause != null && seen.add(cause); cause = cause.getCause())
      if (cause instanceof ThreadDeath) return true;
    return false;
  }
  public static void main(String[] args) throws Exception {
    RuntimeEnvironment.configure(args);
    run();
  }
  public static Activity launch() throws Exception {
    if (Looper.myLooper() == null)
      Looper.prepareMainLooper();
    if (Looper.myLooper() != Looper.getMainLooper())
      throw new IllegalStateException("Launch must run on Android main thread");
    var env = RuntimeEnvironment.get();
    Thread.currentThread().setContextClassLoader(env.loader);
    Components.bindApplication();
    RuntimeEnvironment.trace(
        "Application.construct(" + env.manifest.info.applicationInfo.className + ")");
    env.application =
        (Application) env.loader.loadClass(env.manifest.info.applicationInfo.className)
            .getDeclaredConstructor()
            .newInstance();
    Components.attachApplication(env.application);
    HostDocuments.installIfGranted();
    for (var provider : env.manifest.providers) {
      // ActivityThread installs only this process's enabled providers (and multiprocess ones);
      // a provider declared for another process, such as an SDK's ":isolate", starts there.
      if (!provider.enabled || !provider.multiprocess
          && !java.util.Objects.equals(provider.processName, env.manifest.info.applicationInfo.processName))
        continue;
      var instance = (ContentProvider) env.loader.loadClass(provider.name)
                         .getDeclaredConstructor()
                         .newInstance();
      instance.attachInfo(env.application, provider);
      for (String authority : provider.authority.split(";"))
        if (!authority.isEmpty()) env.contentProviders.put(authority, instance);
    }
    env.application.onCreate();
    Intent intent = env.packageManager.getLaunchIntentForPackage(env.manifest.info.packageName);
    if (intent == null)
      throw new ActivityNotFoundException("Manifest has no MAIN/LAUNCHER or VR activity");
    return startNow(initialLaunchIntent(intent, RuntimeEnvironment.host().coldLaunchIntent(),
        System.getProperty("venture.launchExtras")));
  }
  private static void installFailureHandler() {
    Thread.setDefaultUncaughtExceptionHandler((thread, failure) -> {
      if (isThreadExit(failure)) return;
      failureStatus = 1;
      // Android's AndroidRuntime names the thread and exception in the log
      // before the process dies; the launcher quotes it as the stop reason.
      try {
        RuntimeEnvironment.host().log(6, "AndroidRuntime", "FATAL EXCEPTION: " + thread.getName() + ": " + failure);
      } catch (RuntimeException | Error ignored) {}
      failure.printStackTrace();
      requestShutdown();
    });
  }
  public static void run() throws Exception {
    installFailureHandler();
    try {
      launch();
      if (shutdownRequested && !taskRemoval) shutdown();
      Looper.loop();
    } catch (ThreadDeath exited) {
      // Guest pthread_exit/process exit and native interruption terminate this
      // Java entry too. The runtime retains any nonzero process exit status.
    } catch (Exception | Error failure) {
      if (isThreadExit(failure)) return;
      failureStatus = 1;
      // A game's onDestroy may terminate its own process. Report the original
      // startup failure before entering that callback so it cannot be hidden.
      failure.printStackTrace();
      throw failure;
    } finally {
      // A removed task never finishes: the OS stops this thread with the process.
      if (!taskRemoval) shutdown();
    }
  }
  // Host quit arrives after XR host stop; finish delivers focus loss, pause, stop/save drain, surface detach, then destroy.
  public static boolean requestShutdown() {
    shutdownRequested = true;
    var main = Looper.getMainLooper();
    if (main == null) return false;
    return new Handler(main).post(AppMain::shutdown);
  }
  // A Quest Universal Menu quit removes the task: the Activity pauses and stops
  // without finishing, then the OS ends the process. Unity documents that its
  // quit callbacks do not run there (issues 1320648, UUM-18935, By Design), so
  // isFinishing must stay false and onDestroy must not be delivered.
  public static boolean requestTaskRemoval() {
    taskRemoval = shutdownRequested = true;
    var main = Looper.getMainLooper();
    if (main == null) return false;
    return new Handler(main).post(AppMain::removeTask);
  }
  private static void removeTask() {
    for (Activity activity : new java.util.ArrayList<>(activities)) {
      quitStages.putIfAbsent(activity, 0);
      try { Components.stop(activity); }
      catch (RuntimeException | Error exited) {
        if (!isThreadExit(exited)) throw exited;
      }
      quitStage(activity, 4);
    }
  }
  // ActivityThread-style durability boundary; host quit can additionally use
  // QueuedWork.writesQuiet to wait for writes made after a modern app's pause.
  public static boolean drainQueuedWork() { return QueuedWork.finishPendingWrites(); }
  public static void shutdown() {
    shutdownRequested = true;
    if (Looper.getMainLooper() == null) return;
    if (!Looper.getMainLooper().isCurrentThread()) {
      new Handler(Looper.getMainLooper()).post(AppMain::shutdown);
      return;
    }
    var ending = new java.util.ArrayList<>(activities);
    if (!MENU_QUIT) activities.clear();
    if (MENU_QUIT) for (Activity activity : ending)
      if (quitStages.putIfAbsent(activity, 0) == null && nativeGlue(activity)) ++quitNativeCount;
    try {
      for (Activity a : ending) {
        try { a.finish(); }
        catch (RuntimeException | Error exited) {
          if (!isThreadExit(exited)) throw exited;
        }
      }
    } finally {
      // finish() schedules destruction. Keep the Looper accepting the game's
      // teardown jobs until activityFinished reports the final callback return.
      if (!MENU_QUIT || activities.isEmpty()) {
        try { LocalServices.shutdown(); }
        finally {
          try { drainQueuedWork(); }
          finally { quitMainLooper(); }
        }
      }
    }
  }
  // Ends run()'s Looper.loop. AOSP's main Looper (the 2D framework's) refuses
  // to quit, as ActivityThread ends the process instead; Venture's main returns
  // to the runtime, so it lets the queue quit.
  private static void quitMainLooper() {
    Looper main = Looper.getMainLooper();
    try {
      main.quitSafely();
    } catch (IllegalStateException notQuittable) {
      try {
        java.lang.reflect.Field allowed = MessageQueue.class.getDeclaredField("mQuitAllowed");
        allowed.setAccessible(true);
        allowed.setBoolean(main.getQueue(), true);
      } catch (ReflectiveOperationException | RuntimeException e) {
        notQuittable.addSuppressed(e);
        throw notQuittable;
      }
      main.quitSafely();
    }
  }
  public static void activityFinished(Activity activity) {
    boolean wasTop = activities.peek() == activity;
    boolean removed = activities.remove(activity);
    Components.deliverResult(activity);
    if (removed && activities.isEmpty())
      requestShutdown();
    else if (removed && wasTop)
      Components.resume(activities.peek());
  }
  public static void startActivity(Intent intent) {
    startActivityForResult(null, intent, -1);
  }
  public static void startActivityForResult(Activity caller, Intent intent, int request) {
    if (intent.getComponent() == null
        || !intent.getComponent().getPackageName().equals(
            RuntimeEnvironment.get().manifest.info.packageName))
      throw new ActivityNotFoundException("Only explicit in-process activities are available");
    Intent snapshot = new Intent(intent);
    // AOSP frameworks/base/core/java/android/content/Intent.java:
    // NEW_TASK | CLEAR_TASK replaces the old task with an empty one.
    // UE4 uses the canonical package launcher intent after building its GL
    // program-binary cache; never reuse that engine's native globals in-process.
    final int cold = Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK;
    boolean relaunch = (snapshot.getFlags() & cold) == cold;
    if (relaunch) pendingRelaunch.set(snapshot);
    new Handler(Looper.getMainLooper()).post(() -> {
      if (relaunch) {
        // Delivery can race the app's Runtime.exit. Either ordering must
        // preserve the cold launcher task; do not construct a second native
        // activity inside the old engine/JVM while waiting for that exit.
        if (pendingRelaunch.get() == snapshot) NativeLoader.exit(0);
        return;
      }
      if (shutdownRequested) return;
      try {
        startNow(snapshot, caller, request);
      } catch (ReflectiveOperationException e) {
        throw new IllegalStateException(e);
      }
    });
  }
  private static Activity startNow(Intent intent) throws ReflectiveOperationException {
    return startNow(intent, null, -1);
  }
  private static Activity startNow(Intent intent, Activity caller, int request) throws ReflectiveOperationException {
    var env = RuntimeEnvironment.get();
    String name = intent.getComponent().getClassName();
    var matches = env.manifest.resolveActivities(intent, 0);
    if (matches.isEmpty())
      throw new ActivityNotFoundException("Undeclared activity " + name);
    RuntimeEnvironment.trace("Activity.construct(" + name + ")");
    Activity next = Components.createActivity(intent, matches.get(0).activityInfo, caller, request);
    Activity previous = activities.peek();
    if (previous != null)
      Components.pause(previous);
    activities.push(next);
    try {
      Components.launch(next);
      if (activities.peek() == next && previous != null)
        Components.stop(previous);
    } catch (ThreadDeath exited) {
      throw exited;
    } catch (RuntimeException | Error e) {
      if (isThreadExit(e)) throw e;
      failureStatus = 1;
      e.printStackTrace();
      activities.remove(next);
      try { next.finish(); }
      catch (RuntimeException | Error cleanup) { e.addSuppressed(cleanup); }
      if (previous != null)
        Components.resume(previous);
      throw e;
    }
    return next;
  }
}
