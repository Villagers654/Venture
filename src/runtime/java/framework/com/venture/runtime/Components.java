package com.venture.runtime;

import android.app.Activity;
import android.app.ActivityThread;
import android.app.Application;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;

/**
 * The component model AppMain, LocalServices and the host services drive: the
 * framework's own Context and Activity classes. A 2D process runs layoutlib's
 * Android framework instead and replaces this class with one that drives AOSP's
 * Activity and Context (src/runtime/layoutlib/java, docs/design/layoutlib.md).
 */
public final class Components {
  private Components() {}

  /** Before the application's environment is created. */
  public static void configureProcess() {}

  /** After the application's environment is created. */
  public static void configured(RuntimeEnvironment env) {}

  /** A base context for the application and for services. */
  public static Context newContext() { return new Context(); }

  /** The Activity or Service whose receivers and connections a context's calls belong to. */
  public static Context outerContext(Context context) { return context.ventureOuterContext(); }

  public static void removeReceiverRegistrations(Context owner) {
    Context.ventureRemoveReceiverRegistrations(owner);
  }

  public static void clearReceiverRegistrations() { Context.ventureClearReceiverRegistrations(); }

  public static void bindApplication() {
    ActivityThread.ventureAttach();
    ActivityThread.currentActivityThread().ventureBindApplication(new Context());
  }

  public static void attachApplication(Application application) { application.ventureAttach(new Context()); }

  /** Constructs and attaches the activity an intent resolved to; it is not launched yet. */
  public static Activity createActivity(Intent intent, ActivityInfo resolved, Activity caller, int request)
      throws ReflectiveOperationException {
    var env = RuntimeEnvironment.get();
    String className = resolved.targetActivity == null ? intent.getComponent().getClassName() : resolved.targetActivity;
    Activity activity = (Activity) env.loader.loadClass(className).getDeclaredConstructor().newInstance();
    activity.ventureAttach(env.application);
    activity.setIntent(intent);
    activity.ventureSetResultCaller(caller, request);
    String name = intent.getComponent().getClassName();
    for (var info : env.manifest.activities)
      if (info.name.equals(name) && info.theme != 0) {
        activity.setTheme(info.theme);
        break;
      }
    return activity;
  }

  public static void launch(Activity activity) { activity.performLaunch(); }
  public static void pause(Activity activity) { activity.performPause(); }
  public static void stop(Activity activity) { activity.performStop(); }
  public static void resume(Activity activity) { activity.performResume(); }
  public static void deliverResult(Activity activity) { activity.ventureDeliverResult(); }

  public static void displayPeriodChanged(long period) { android.view.Choreographer.displayPeriodChanged(period); }

  public static android.view.Display defaultDisplay() { return new android.view.Display(); }

  // A VR game's process has no window compositor: a virtual display there keeps
  // its Surface for the app (a recorder reads it) and draws no windows.
  public static android.view.Display virtualDisplay(int id, String name, int width, int height, int densityDpi,
      int flags, android.view.Surface surface) {
    return new android.view.Display();
  }
  public static void virtualDisplayChanged(int id, android.view.Surface surface, int width, int height, int densityDpi) {}
  public static void virtualDisplayReleased(int id) {}
}
