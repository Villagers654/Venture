package android.app;
import android.content.*;
import android.os.*;
import android.view.*;
import com.venture.runtime.*;
public class Activity extends ContextWrapper implements KeyEvent.Callback, Window.Callback {
  // One hosted task owns one logical display; resizing its desktop mirror does
  // not enter Android split-screen or picture-in-picture mode.
  public boolean isInMultiWindowMode() { return false; }
  public boolean isInPictureInPictureMode() { return false; }
  public static final int RESULT_CANCELED = 0, RESULT_OK = -1, RESULT_FIRST_USER = 1;
  private Activity resultCaller;
  private int resultRequest = -1, resultCode = RESULT_CANCELED;
  private Intent resultData;
  public String getCallingPackage() { return resultCaller == null ? null : resultCaller.getPackageName(); }
  public ComponentName getCallingActivity() { return resultCaller == null ? null : resultCaller.getComponentName(); }
  public void startActivityForResult(Intent intent, int request) {
    AppMain.startActivityForResult(this, intent, request);
  }
  public void startActivityForResult(Intent intent, int request, Bundle options) {
    if (options != null && !options.isEmpty()) throw new UnsupportedOperationException("Activity launch options unavailable");
    startActivityForResult(intent, request);
  }
  public void setResult(int code) { setResult(code, null); }
  public void setResult(int code, Intent data) { resultCode = code; resultData = data; }
  public final void ventureSetResultCaller(Activity caller, int request) {
    if (request >= 0) { resultCaller = caller; resultRequest = request; }
  }
  public final void ventureDeliverResult() {
    Activity caller = resultCaller; resultCaller = null;
    if (caller != null && !caller.isDestroyed())
      caller.onActivityResult(resultRequest, resultCode, resultData == null ? null : new Intent(resultData));
  }
  public void overridePendingTransition(int enter, int exit) {
    android.util.Log.w("Activity", "Window transition animations unavailable");
  }
  private final Window window = new Window(this);
  private final FragmentManager fragments = new FragmentManager(this);
  public FragmentManager getFragmentManager() { return fragments; }
  // AppMain currently creates a fresh task; it does not recreate an activity
  // for a configuration change, so there is no previous retained instance.
  public Object getLastNonConfigurationInstance() { return null; }
  public Object onRetainNonConfigurationInstance() { return null; }
  public boolean isChangingConfigurations() { return false; }
  public int getChangingConfigurations() { return 0; }
  private final java.util.List<Application.ActivityLifecycleCallbacks> callbacks =
      new java.util.concurrent.CopyOnWriteArrayList<>();
  public void registerActivityLifecycleCallbacks(Application.ActivityLifecycleCallbacks callback) {
    callbacks.add(java.util.Objects.requireNonNull(callback));
  }
  public void unregisterActivityLifecycleCallbacks(Application.ActivityLifecycleCallbacks callback) {
    callbacks.remove(callback);
  }
  private java.util.List<Application.ActivityLifecycleCallbacks> lifecycleCallbacks() {
    var all = new java.util.ArrayList<>(getApplication().lifecycleCallbacks());
    all.addAll(callbacks);
    return all;
  }
  private Intent intent = new Intent(Intent.ACTION_MAIN);
  private volatile boolean finished;
  private boolean created, started, resumed, destroyed, postCreated;
  private int windowGeneration;
  private boolean windowFocused;
  private int orientation = -1;
  private CharSequence title = "";
  private long fullyDrawnAt = -1;
  public void reportFullyDrawn() {
    if (fullyDrawnAt >= 0) return;
    fullyDrawnAt = SystemClock.uptimeMillis();
    android.util.Log.i("Activity", getClass().getName() + " reported fully drawn at " + fullyDrawnAt + " ms");
  }
  public Activity() {
    super(null);
  }
  public final void ventureAttach(android.content.Context context) {
    attachBaseContext(new com.venture.runtime.ComponentContext(context, this));
  }
  protected void onCreate(Bundle state) {
    RuntimeEnvironment.trace("Activity.onCreate");
  }
  protected void onStart() {
    RuntimeEnvironment.trace("Activity.onStart");
  }
  protected void onResume() {
    RuntimeEnvironment.trace("Activity.onResume");
  }
  protected void onRestart() {}
  protected void onPause() {}
  protected void onStop() {}
  protected void onDestroy() {}
  protected void onPostCreate(Bundle state) {}
  protected void onPostResume() {}
  protected void onSaveInstanceState(Bundle state) {}
  // AOSP Activity.java: the persistent-state overload also runs the ordinary hook.
  public void onSaveInstanceState(Bundle state, android.os.PersistableBundle persistentState) {
    onSaveInstanceState(state);
  }
  protected void onNewIntent(Intent i) {
    intent = i;
  }
  protected void onActivityResult(int request, int result, Intent data) {}
  public void onConfigurationChanged(android.content.res.Configuration config) {}
  public void onLowMemory() {}
  public void onTrimMemory(int level) {}
  public Window getWindow() {
    RuntimeEnvironment.trace("Activity.getWindow");
    return window;
  }
  public WindowManager getWindowManager() {
    return (WindowManager) getSystemService(WINDOW_SERVICE);
  }
  @Override public android.view.Display getDisplay() {
    return getWindowManager().getDefaultDisplay();
  }
  public void finishAndRemoveTask() {
    finish();
  }
  public Intent getIntent() {
    RuntimeEnvironment.trace("Activity.getIntent");
    return intent;
  }
  public void setIntent(Intent i) {
    intent = i;
  }
  public Application getApplication() {
    return RuntimeEnvironment.get().application;
  }
  public ComponentName getComponentName() {
    return new ComponentName(getPackageName(), getClass().getName());
  }
  public String getLocalClassName() {
    // AOSP Activity.java: strip the application package only at a dot boundary.
    String pkg = getPackageName(), cls = getComponentName().getClassName();
    int length = pkg.length();
    return cls.startsWith(pkg) && cls.length() > length && cls.charAt(length) == '.'
        ? cls.substring(length + 1) : cls;
  }
  public SharedPreferences getPreferences(int mode) {
    return getSharedPreferences(getLocalClassName(), mode);
  }
  public Intent getParentActivityIntent() {
    try {
      String parent = getPackageManager().getActivityInfo(getComponentName(), 0).parentActivityName;
      if (parent == null) return null;
      ComponentName component = new ComponentName(getPackageName(), parent);
      Intent result = new Intent().setComponent(component);
      if (getPackageManager().getActivityInfo(component, 0).parentActivityName == null)
        result.setAction(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER);
      return result;
    } catch (android.content.pm.PackageManager.NameNotFoundException e) {
      throw new IllegalStateException("Invalid parent activity declaration", e);
    }
  }
  public boolean requestWindowFeature(int f) {
    return window.requestFeature(f);
  }
  public void setContentView(View v) {
    window.setContentView(v);
  }
  public LayoutInflater getLayoutInflater() {
    return window.getLayoutInflater();
  }
  public void setContentView(int resource) {
    window.setContentView(resource);
  }
  public void setContentView(View v, ViewGroup.LayoutParams p) {
    window.setContentView(v, p);
  }
  public <T extends View> T findViewById(int id) {
    return window.getDecorView().findViewById(id);
  }
  public void addContentView(View v, ViewGroup.LayoutParams p) {
    ((ViewGroup) window.getDecorView()).addView(v, p);
  }
  public void runOnUiThread(Runnable r) {
    if (getMainLooper().isCurrentThread())
      r.run();
    else
      new Handler(getMainLooper()).post(r);
  }
  public int getRequestedOrientation() {
    return orientation;
  }
  public void setRequestedOrientation(int o) {
    orientation = o;
  }
  public void setTitle(CharSequence t) {
    title = t;
  }
  public CharSequence getTitle() {
    return title;
  }
  public void setVolumeControlStream(int stream) {
    RuntimeEnvironment.trace("Activity.setVolumeControlStream(" + stream + ")");
  }
  public boolean isFinishing() {
    return finished;
  }
  public boolean isDestroyed() {
    return destroyed;
  }
  public void finish() {
    if (!AppMain.MENU_QUIT) {
      if (!getMainLooper().isCurrentThread()) {
        new Handler(getMainLooper()).post(this::finish);
        return;
      }
      if (!finished) { finished = true; performFinish(); }
      return;
    }
    synchronized (this) {
      if (finished) return;
      finished = true;
    }
    // AOSP Activity.finish requests ActivityClient.finishActivity; destruction
    // arrives later through ActivityThread. Never run application destruction
    // reentrantly inside the callback that requested finish, even on main.
    if (AppMain.QUIT_DIAGNOSTICS) AppMain.quitLog("Activity finish requested activity=" + getClass().getName());
    new Handler(getMainLooper()).post(this::performFinish);
  }
  private void performFinish() {
    if (!destroyed) {
      try {
        performStop();
        if (created) {
          fragments.destroy();
          for (var c : lifecycleCallbacks()) c.onActivityPreDestroyed(this);
          if (AppMain.QUIT_DIAGNOSTICS) AppMain.quitLog("Activity onDestroy begin activity=" + getClass().getName());
          AppMain.quitStage(this, 4);
          onDestroy();
          if (AppMain.QUIT_DIAGNOSTICS) AppMain.quitLog("Activity onDestroy returned activity=" + getClass().getName());
          for (var c : lifecycleCallbacks()) c.onActivityDestroyed(this);
          for (var c : lifecycleCallbacks()) c.onActivityPostDestroyed(this);
        }
      } finally { LocalServices.removeContextRegistrations(this); }
      destroyed = true;
      AppMain.activityFinished(this);
    }
  }
  public void onBackPressed() {
    finish();
  }
  public boolean moveTaskToBack(boolean nonRoot) {
    return false;
  }
  public void onWindowFocusChanged(boolean focus) {
    window.getDecorView().onWindowFocusChanged(focus);
  }
  public boolean dispatchKeyEvent(KeyEvent e) {
    return e.getAction() == 0 ? onKeyDown(e.getKeyCode(), e) : onKeyUp(e.getKeyCode(), e);
  }
  public boolean onKeyDown(int code, KeyEvent e) {
    return false;
  }
  public boolean onKeyUp(int code, KeyEvent e) {
    return false;
  }
  public boolean onKeyLongPress(int c, KeyEvent e) {
    return false;
  }
  public boolean onKeyMultiple(int c, int n, KeyEvent e) {
    return false;
  }
  public boolean onTouchEvent(MotionEvent e) {
    return false;
  }
  public boolean onGenericMotionEvent(MotionEvent e) {
    return false;
  }
  public void requestPermissions(String[] permissions, int request) {
    int[] results =
        java.util.Arrays.stream(permissions).mapToInt(this::checkSelfPermission).toArray();
    new Handler(getMainLooper())
        .post(() -> onRequestPermissionsResult(request, permissions, results));
  }
  public void onRequestPermissionsResult(int request, String[] permissions, int[] results) {}
  public boolean shouldShowRequestPermissionRationale(String p) {
    return false;
  }
  public final void performLaunch() {
    if (created || finished)
      throw new IllegalStateException("Activity already launched");
    created = true;
    for (var c : lifecycleCallbacks()) c.onActivityPreCreated(this, null);
    onCreate(null);
    fragments.dispatch(1);
    for (var c : lifecycleCallbacks()) c.onActivityCreated(this, null);
    for (var c : lifecycleCallbacks()) c.onActivityPostCreated(this, null);
    if (!finished)
      performResume();
  }
  public final void performResume() {
    if (finished || resumed)
      return;
    if (!started) {
      started = true;
      for (var c : lifecycleCallbacks()) c.onActivityPreStarted(this);
      onStart();
      fragments.dispatch(2);
      for (var c : lifecycleCallbacks()) c.onActivityStarted(this);
      for (var c : lifecycleCallbacks()) c.onActivityPostStarted(this);
    }
    if (!postCreated) {
      postCreated = true;
      onPostCreate(null);
    }
    resumed = true;
    for (var c : lifecycleCallbacks()) c.onActivityPreResumed(this);
    onResume();
    fragments.dispatch(3);
    for (var c : lifecycleCallbacks()) c.onActivityResumed(this);
    for (var c : lifecycleCallbacks()) c.onActivityPostResumed(this);
    onPostResume();
    // AOSP 12L ViewRootImpl.scheduleTraversals uses Choreographer, and
    // windowFocusChanged posts a separate main-Looper message. Surface/focus
    // callbacks must not run inside the Activity's resume transaction.
    int generation = ++windowGeneration;
    Choreographer.getInstance().postFrameCallback(time -> {
      if (!resumed || finished || generation != windowGeneration) return;
      window.attach();
      new Handler(Looper.getMainLooper()).post(() -> {
        if (!resumed || finished || generation != windowGeneration) return;
        windowFocused = true;
        onWindowFocusChanged(true);
      });
    });
  }
  public final void performPause() {
    if (!resumed)
      return;
    ++windowGeneration;
    if (windowFocused) {
      windowFocused = false;
      onWindowFocusChanged(false);
    }
    resumed = false;
    fragments.dispatch(2);
    for (var c : lifecycleCallbacks()) c.onActivityPrePaused(this);
    if (AppMain.QUIT_DIAGNOSTICS) AppMain.quitLog("Activity onPause begin activity=" + getClass().getName());
    onPause();
    if (AppMain.QUIT_DIAGNOSTICS) AppMain.quitLog("Activity onPause returned activity=" + getClass().getName());
    for (var c : lifecycleCallbacks()) c.onActivityPaused(this);
    for (var c : lifecycleCallbacks()) c.onActivityPostPaused(this);
    // ActivityThread.handlePauseActivity: only pre-Honeycomb apps drain here.
    if (getApplicationInfo().targetSdkVersion < 11 && AppMain.drainQueuedWork()) AppMain.quitStage(this, 2);
  }
  public final void performStop() {
    performPause();
    if (started) {
      started = false;
      fragments.dispatch(1);
      for (var c : lifecycleCallbacks()) c.onActivityPreStopped(this);
      if (AppMain.QUIT_DIAGNOSTICS) AppMain.quitLog("Activity onStop begin activity=" + getClass().getName());
      onStop();
      if (AppMain.QUIT_DIAGNOSTICS) AppMain.quitLog("Activity onStop returned activity=" + getClass().getName());
      for (var c : lifecycleCallbacks()) c.onActivityStopped(this);
      for (var c : lifecycleCallbacks()) c.onActivityPostStopped(this);
    }
    AppMain.quitStage(this, 1);
    // AOSP android-12.1.0_r1 ActivityThread.java: handleStopActivity calls
    // performStopActivityInner before updateVisibility(false). Keep the
    // NativeActivity window through its onStop callback.
    if (AppMain.QUIT_DIAGNOSTICS) AppMain.quitLog("Activity surface detach begin activity=" + getClass().getName());
    window.detach();
    if (AppMain.QUIT_DIAGNOSTICS) AppMain.quitLog("Activity surface detach returned activity=" + getClass().getName());
    if (getApplicationInfo().targetSdkVersion >= 11 && AppMain.drainQueuedWork()) AppMain.quitStage(this, 2);
  }
}
