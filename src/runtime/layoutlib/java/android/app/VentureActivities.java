package android.app;

import android.content.Context;
import android.content.ContextWrapper;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.res.Configuration;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Looper;
import com.venture.runtime.AppMain;
import com.venture.runtime.RuntimeEnvironment;
import com.venture.runtime.layoutlib.HostContext;
import com.venture.runtime.layoutlib.Layoutlib;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.IdentityHashMap;
import java.util.Map;

/**
 * Runs AOSP's Activity and Application in a 2D process the way ActivityThread
 * does on a device: attach with a base context, then perform the lifecycle in
 * order. Activity manager calls the activities make (finish, start, move to
 * back) come here through Venture's Instrumentation and an in-process
 * IActivityClientController, and go on to AppMain's activity stack.
 */
public final class VentureActivities {
  private VentureActivities() {}

  private enum State { CREATED, STARTED, RESUMED, PAUSED, STOPPED, DESTROYED }

  private static final class Record {
    final Activity activity;
    final IBinder token = new Binder();
    final Activity caller;
    final String callerWho;
    final int request;
    State state = State.CREATED;
    Record(Activity activity, Activity caller, String callerWho, int request) {
      this.activity = activity;
      this.caller = caller;
      this.callerWho = callerWho;
      this.request = request;
    }
  }

  private static final Map<Activity, Record> records = new IdentityHashMap<>();
  private static final Map<IBinder, Record> byToken = new IdentityHashMap<>();
  private static ActivityThread thread;
  private static final Instrumentation instrumentation = new VentureInstrumentation();
  // The caller and fragment of the next activity started for a result.
  private static String pendingWho;

  private static Record record(Activity activity) {
    Record record = records.get(activity);
    if (record == null) throw new IllegalStateException("Not a hosted activity: " + activity);
    return record;
  }

  /** ActivityThread.bindApplication: the process's ActivityThread and its activity manager links. */
  public static void bindApplication() {
    if (thread != null) return;
    thread = new ActivityThread();
    try {
      java.lang.reflect.Field current = ActivityThread.class.getDeclaredField("sCurrentActivityThread");
      current.setAccessible(true);
      current.set(null, thread);
    } catch (ReflectiveOperationException e) {
      throw new IllegalStateException(e);
    }
    ActivityClient.setActivityClientController(proxy(IActivityClientController.class, VentureActivities::activityClient));
  }

  public static void attachApplication(Application application) {
    attachBaseContext(application, new HostContext());
    thread.mInitialApplication = application;
    thread.mAllApplications.add(application);
  }

  static void attachBaseContext(ContextWrapper wrapper, Context base) {
    try {
      Method attach = ContextWrapper.class.getDeclaredMethod("attachBaseContext", Context.class);
      attach.setAccessible(true);
      attach.invoke(wrapper, base);
    } catch (ReflectiveOperationException e) {
      throw new IllegalStateException(e);
    }
  }

  /** Constructs and attaches an activity; it is launched separately. */
  public static Activity create(Intent intent, ActivityInfo info, Activity caller, int request)
      throws ReflectiveOperationException {
    var env = RuntimeEnvironment.get();
    String className = info.targetActivity == null ? intent.getComponent().getClassName() : info.targetActivity;
    Activity activity = instrumentation.newActivity(env.loader, className, intent);
    String who = pendingWho;
    pendingWho = null;
    Record record = new Record(activity, caller, who, request);
    records.put(activity, record);
    byToken.put(record.token, record);
    Context base = new HostContext(activity, Layoutlib.display());
    Configuration config = new Configuration(env.resources.getConfiguration());
    CharSequence title = info.loadLabel(env.packageManager);
    activity.attach(base, thread, instrumentation, record.token, 0, env.application, intent, info, title, null,
        null, null, config, null, null, null, null, null, null, null);
    int theme = info.getThemeResource();
    if (theme != 0) activity.setTheme(theme);
    return activity;
  }

  /** handleLaunchActivity, handleStartActivity and handleResumeActivity. */
  public static void launch(Activity activity) {
    Record record = record(activity);
    activity.mCalled = false;
    activity.performCreate(null);
    record.state = State.CREATED;
    if (activity.mFinished) return;
    activity.performStart("launch");
    record.state = State.STARTED;
    instrumentation.callActivityOnPostCreate(activity, null);
    if (activity.mFinished) return;
    resume(activity);
  }

  public static void resume(Activity activity) {
    Record record = record(activity);
    if (record.state == State.RESUMED || record.state == State.DESTROYED) return;
    if (record.state == State.STOPPED) {
      // Activity.performRestart: setStoppedState(token, false) first.
      com.venture.runtime.layoutlib.Panel.activityStopped(activity, false);
      activity.performRestart(true);
      record.state = State.STARTED;
    }
    activity.performResume(false, "resume");
    record.state = State.RESUMED;
    com.venture.runtime.layoutlib.Panel.activityResumed(activity, true);
    if (activity.mFinished) return;
    if (activity.mDecor == null) {
      activity.mDecor = activity.getWindow().getDecorView();
    }
    if (activity.mVisibleFromClient) activity.makeVisible();
  }

  public static void pause(Activity activity) {
    Record record = record(activity);
    if (record.state != State.RESUMED) return;
    // The task leaving the front takes window focus before onPause.
    com.venture.runtime.layoutlib.Panel.activityResumed(activity, false);
    activity.performPause();
    record.state = State.PAUSED;
    // ActivityThread.handlePauseActivity: only pre-Honeycomb apps drain here.
    if (activity.getApplicationInfo().targetSdkVersion < 11 && AppMain.drainQueuedWork()) AppMain.quitStage(activity, 2);
  }

  public static void stop(Activity activity) {
    Record record = record(activity);
    pause(activity);
    if (record.state != State.PAUSED && record.state != State.STARTED) return;
    // Activity.performStop: setStoppedState(token, true) before onStop. The
    // activity's windows, its dialogs' and its presentations' stop drawing.
    com.venture.runtime.layoutlib.Panel.activityStopped(activity, true);
    activity.performStop(false, "stop");
    record.state = State.STOPPED;
    // Quit progress as the classic Activity reports it: stop returned, then
    // ActivityThread.handleStopActivity's QueuedWork drain.
    AppMain.quitStage(activity, 1);
    if (activity.mDecor != null) activity.mDecor.setVisibility(android.view.View.INVISIBLE);
    if (activity.getApplicationInfo().targetSdkVersion >= 11 && AppMain.drainQueuedWork()) AppMain.quitStage(activity, 2);
  }

  static void destroy(Activity activity) {
    Record record = record(activity);
    if (record.state == State.DESTROYED) return;
    stop(activity);
    AppMain.quitStage(activity, 4);
    activity.performDestroy();
    record.state = State.DESTROYED;
    if (activity.mDecor != null && activity.mWindowAdded) {
      activity.getWindowManager().removeViewImmediate(activity.mDecor);
      activity.mWindowAdded = false;
    }
    activity.mDecor = null;
    com.venture.runtime.layoutlib.Panel.activityDestroyed(activity);
    records.remove(activity);
    byToken.remove(record.token);
    HostContext.forgetReceivers(activity);
  }

  /** The finished activity's result, to the activity that started it for one. */
  public static void deliverResult(Activity activity) {
    Record record = records.get(activity);
    if (record == null) record = finishedRecords.remove(activity);
    if (record == null || record.caller == null || record.request < 0) return;
    Activity caller = record.caller;
    int code;
    Intent data;
    synchronized (activity) {
      code = activity.mResultCode;
      data = activity.mResultData;
    }
    caller.dispatchActivityResult(record.callerWho, record.request, code, data, "result");
  }

  private static final Map<Activity, Record> finishedRecords = new IdentityHashMap<>();

  // IActivityClientController: the calls an Activity makes to the activity manager.
  private static Object activityClient(Method method, Object[] args) {
    switch (method.getName()) {
      case "finishActivity": {
        Record record = byToken.get((IBinder) args[0]);
        if (record == null) return false;
        new android.os.Handler(Looper.getMainLooper()).post(() -> {
          Activity activity = record.activity;
          finishedRecords.put(activity, record);
          destroy(activity);
          AppMain.activityFinished(activity);
          finishedRecords.remove(activity);
        });
        return true;
      }
      case "moveActivityTaskToBack":
        return false;
      case "getTaskForActivity":
        return 1;
      case "getCallingActivity": {
        Record record = byToken.get((IBinder) args[0]);
        return record == null || record.caller == null ? null : record.caller.getComponentName();
      }
      case "getCallingPackage": {
        Record record = byToken.get((IBinder) args[0]);
        return record == null || record.caller == null ? null : record.caller.getPackageName();
      }
      default:
        return null;
    }
  }

  /** An in-process implementation of a system service interface; other calls return defaults. */
  public static <T> T proxy(Class<T> type, java.util.function.BiFunction<Method, Object[], Object> calls) {
    InvocationHandler handler = (self, method, args) -> {
      if (method.getDeclaringClass() == Object.class) {
        return switch (method.getName()) {
          case "hashCode" -> System.identityHashCode(self);
          case "equals" -> self == args[0];
          default -> type.getSimpleName() + "@venture";
        };
      }
      if (method.getName().equals("asBinder")) return new Binder();
      Object result = calls.apply(method, args == null ? new Object[0] : args);
      if (result != null) return result;
      Class<?> returns = method.getReturnType();
      if (returns == boolean.class) return false;
      if (returns == int.class) return 0;
      if (returns == long.class) return 0L;
      if (returns == float.class) return 0f;
      if (returns == double.class) return 0d;
      if (returns == byte.class) return (byte) 0;
      if (returns == short.class) return (short) 0;
      if (returns == char.class) return (char) 0;
      return null;
    };
    return type.cast(Proxy.newProxyInstance(type.getClassLoader(), new Class<?>[] {type}, handler));
  }

  /** Starts activities in Venture's activity stack; the system's activity manager is not there. */
  private static final class VentureInstrumentation extends Instrumentation {
    @Override
    public ActivityResult execStartActivity(Context who, IBinder contextThread, IBinder token, Activity target,
                                            Intent intent, int requestCode, Bundle options) {
      start(who, target, null, intent, requestCode);
      return null;
    }

    @Override
    public ActivityResult execStartActivity(Context who, IBinder contextThread, IBinder token, String target,
                                            Intent intent, int requestCode, Bundle options) {
      Record record = byToken.get(token);
      start(who, record == null ? null : record.activity, target, intent, requestCode);
      return null;
    }

    @Override
    public ActivityResult execStartActivity(Context who, IBinder contextThread, IBinder token, String target,
                                            Intent intent, int requestCode, Bundle options,
                                            android.os.UserHandle user) {
      return execStartActivity(who, contextThread, token, target, intent, requestCode, options);
    }

    private static void start(Context who, Activity caller, String callerWho, Intent intent, int requestCode) {
      if (SystemActivities.start(who, caller, callerWho, intent, requestCode)) return;
      if (intent.getComponent() == null) {
        var resolved = who.getPackageManager().resolveActivity(intent, 0);
        if (resolved != null && resolved.activityInfo != null)
          intent = new Intent(intent).setClassName(resolved.activityInfo.packageName, resolved.activityInfo.name);
      }
      pendingWho = callerWho;
      AppMain.startActivityForResult(requestCode >= 0 ? caller : null, intent, requestCode);
    }
  }
}
