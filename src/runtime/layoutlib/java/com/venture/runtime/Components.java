package com.venture.runtime;

import android.app.Activity;
import android.app.Application;
import android.app.VentureActivities;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import com.venture.runtime.layoutlib.HostContext;
import com.venture.runtime.layoutlib.Layoutlib;

/**
 * The component model of a 2D process: AOSP's Activity, Application and Context
 * from layoutlib, run by android.app.VentureActivities, with HostContext as
 * their base context. Replaces the classic framework's class of this name
 * (src/runtime/java/framework/com/venture/runtime/Components.java).
 */
public final class Components {
  private Components() {}

  /** layoutlib's natives load before the application's AssetManager is made. */
  public static void configureProcess() { Layoutlib.start(); }

  /** The app's resources: its APK over the framework's, on the 2D panel's display. */
  public static void configured(RuntimeEnvironment env) {
    env.assets.addAssetPath(env.apk.getPath());
    Layoutlib.configureResources(env.resources);
  }

  public static Context newContext() { return new HostContext(); }

  public static Context outerContext(Context context) { return HostContext.outerOf(context); }

  public static void removeReceiverRegistrations(Context owner) { HostContext.forgetReceivers(HostContext.outerOf(owner)); }

  public static void clearReceiverRegistrations() { HostContext.forgetReceivers(null); }

  public static void bindApplication() {
    Layoutlib.configureResources(RuntimeEnvironment.resources());
    VentureActivities.bindApplication();
  }

  public static void attachApplication(Application application) { VentureActivities.attachApplication(application); }

  public static Activity createActivity(Intent intent, ActivityInfo resolved, Activity caller, int request)
      throws ReflectiveOperationException {
    return VentureActivities.create(intent, resolved, caller, request);
  }

  public static void launch(Activity activity) { VentureActivities.launch(activity); }
  public static void pause(Activity activity) { VentureActivities.pause(activity); }
  public static void stop(Activity activity) { VentureActivities.stop(activity); }
  public static void resume(Activity activity) { VentureActivities.resume(activity); }
  public static void deliverResult(Activity activity) { VentureActivities.deliverResult(activity); }

  public static void displayPeriodChanged(long period) {}

  public static android.view.Display defaultDisplay() { return Layoutlib.display(); }

  /** A virtual display: its own panel, composited into the display's Surface. */
  public static android.view.Display virtualDisplay(int id, String name, int width, int height, int densityDpi,
      int flags, android.view.Surface surface) {
    android.view.Display display = Layoutlib.virtualDisplay(id, name, width, height, densityDpi, flags);
    com.venture.runtime.layoutlib.Panel.createForDisplay(display, width, height, surface);
    return display;
  }
  public static void virtualDisplayChanged(int id, android.view.Surface surface, int width, int height, int densityDpi) {
    com.venture.runtime.layoutlib.Panel panel = com.venture.runtime.layoutlib.Panel.forDisplay(id);
    if (panel != null) panel.output(surface, width, height);
  }
  public static void virtualDisplayReleased(int id) { com.venture.runtime.layoutlib.Panel.releaseDisplay(id); }
}
