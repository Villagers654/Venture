package android.content;
import android.database.DatabaseErrorHandler;
import android.database.sqlite.SQLiteDatabase;
import android.content.pm.*;
import android.content.res.*;
import android.os.*;
import com.venture.runtime.*;
import java.io.*;
import java.util.*;
public class Context {
  /** Framework dispatcher identity; component base contexts return their outer component. */
  public Context ventureOuterContext() { return this; }
  public static final int MODE_PRIVATE = 0, MODE_MULTI_PROCESS = 4;
  public static final int BIND_AUTO_CREATE = 1;
  public static final String ACTIVITY_SERVICE = "activity", WINDOW_SERVICE = "window", AUDIO_SERVICE = "audio",
                             POWER_SERVICE = "power", INPUT_METHOD_SERVICE = "input_method",
                             VIBRATOR_SERVICE = "vibrator", VIBRATOR_MANAGER_SERVICE = "vibrator_manager",
                             CONNECTIVITY_SERVICE = "connectivity",
                             TELEPHONY_SERVICE = "phone", CLIPBOARD_SERVICE = "clipboard",
                             BATTERY_SERVICE = "batterymanager", DISPLAY_SERVICE = "display";
  public static final String LOCATION_SERVICE = "location", BLUETOOTH_SERVICE = "bluetooth";
  public static final String SENSOR_SERVICE = "sensor", LAYOUT_INFLATER_SERVICE = "layout_inflater",
      UI_MODE_SERVICE = "uimode";
  public static final String MEDIA_ROUTER_SERVICE = "media_router", INPUT_SERVICE = "input";
  public static final String USER_SERVICE = "user", WIFI_SERVICE = "wifi", JOB_SCHEDULER_SERVICE = "jobscheduler",
      STORAGE_SERVICE = "storage", ALARM_SERVICE = "alarm", NOTIFICATION_SERVICE = "notification",
      CAPTIONING_SERVICE = "captioning", MEDIA_METRICS_SERVICE = "media_metrics", LOCALE_SERVICE = "locale";
  private static final Map<String, SharedPreferences> preferences =
      new java.util.concurrent.ConcurrentHashMap<>();
  // Dynamic receivers match process-wide, as ActivityManagerService does; each
  // registration keeps the Context that registered it and its Handler.
  private static final boolean TRACE_RECEIVERS = System.getenv("VENTURE_TRACE_RECEIVERS") != null;
  private static long nextReceiverId;
  private Handler receiverMainHandler;
  private static final class Registration {
    final Context owner;
    final BroadcastReceiver receiver;
    final Handler scheduler;
    final long id;
    final List<IntentFilter> filters = new java.util.concurrent.CopyOnWriteArrayList<>();
    volatile boolean forgotten;
    Registration(Context owner, BroadcastReceiver receiver, Handler scheduler) {
      this.owner = owner;
      this.receiver = receiver;
      this.scheduler = scheduler;
      id = ++nextReceiverId; // registrations lock
    }
    void trace(String event) {
      if (TRACE_RECEIVERS) android.util.Log.i("BroadcastReceiver", event + " id=" + id);
    }
    void post(Intent intent) {
      Intent copy = new Intent(intent);
      scheduler.post(() -> {
        // LoadedApk's dispatcher forgets pending deliveries on unregister. An
        // onReceive already running may finish; no application code holds our lock.
        if (forgotten) { trace("cancel"); return; }
        trace("deliver");
        receiver.ventureDeliver(owner, copy);
      });
    }
  }
  private static final java.util.List<Registration> registrations =
      new java.util.concurrent.CopyOnWriteArrayList<>();
  private final Map<String, Object> services = new HashMap<>();
  private Resources.Theme theme;
  protected final java.util.List<ComponentCallbacks> componentCallbacks =
      new java.util.concurrent.CopyOnWriteArrayList<>();
  public void registerComponentCallbacks(ComponentCallbacks callback) {
    java.util.Objects.requireNonNull(callback);
    Context app = getApplicationContext();
    if (app != this) app.registerComponentCallbacks(callback);
    else componentCallbacks.add(callback);
  }
  public void unregisterComponentCallbacks(ComponentCallbacks callback) {
    java.util.Objects.requireNonNull(callback);
    Context app = getApplicationContext();
    if (app != this) app.unregisterComponentCallbacks(callback);
    else componentCallbacks.remove(callback);
  }
  public String getPackageName() {
    RuntimeEnvironment.trace("Context.getPackageName");
    return RuntimeEnvironment.get().manifest.info.packageName;
  }
  public Context getApplicationContext() {
    var a = RuntimeEnvironment.get().application;
    return a == null ? this : a;
  }
  public ClassLoader getClassLoader() {
    return RuntimeEnvironment.get().loader;
  }
  public ApplicationInfo getApplicationInfo() {
    RuntimeEnvironment.trace("Context.getApplicationInfo");
    return RuntimeEnvironment.get().manifest.info.applicationInfo;
  }
  public PackageManager getPackageManager() {
    return RuntimeEnvironment.get().packageManager;
  }
  public File getFilesDir() {
    return RuntimeEnvironment.directory(new File(RuntimeEnvironment.get().data, "files"));
  }
  public File getCacheDir() {
    return RuntimeEnvironment.directory(RuntimeEnvironment.get().cache);
  }
  // AOSP ContextImpl: code_cache is a sibling of cache in the app data dir.
  public File getCodeCacheDir() {
    return RuntimeEnvironment.directory(new File(getDataDir(), "code_cache"));
  }
  public File getObbDir() {
    return RuntimeEnvironment.get().obb;
  }
  public File[] getObbDirs() {
    return new File[] {getObbDir()};
  }
  public File getExternalFilesDir(String type) {
    File root = RuntimeEnvironment.directory(RuntimeEnvironment.get().external);
    return type == null ? root : RuntimeEnvironment.directory(child(root, type));
  }
  public File[] getExternalFilesDirs(String t) {
    return new File[] {getExternalFilesDir(t)};
  }
  public File getExternalCacheDir() {
    return RuntimeEnvironment.directory(new File(RuntimeEnvironment.get().data, "external-data/cache"));
  }
  public File getDataDir() {
    return RuntimeEnvironment.directory(RuntimeEnvironment.get().data);
  }
  public File getNoBackupFilesDir() {
    return RuntimeEnvironment.directory(new File(getDataDir(), "no_backup"));
  }
  // AOSP ContextImpl: an absolute name is used as is, a plain name lives in databases/.
  public File getDatabasePath(String name) {
    if (new File(name).isAbsolute() || name.startsWith("/")) return new File(name);
    return child(RuntimeEnvironment.directory(new File(getDataDir(), "databases")), name);
  }
  public SQLiteDatabase openOrCreateDatabase(String name, int mode, SQLiteDatabase.CursorFactory factory) {
    return openOrCreateDatabase(name, mode, factory, null);
  }
  public SQLiteDatabase openOrCreateDatabase(String name, int mode, SQLiteDatabase.CursorFactory factory,
                                             DatabaseErrorHandler errorHandler) {
    return SQLiteDatabase.openOrCreateDatabase(getDatabasePath(name), factory);
  }
  public boolean deleteDatabase(String name) {
    return SQLiteDatabase.deleteDatabase(getDatabasePath(name));
  }
  public String[] databaseList() {
    String[] names = new File(getDataDir(), "databases").list();
    return names != null ? names : new String[0];
  }
  public File getDir(String name, int mode) {
    return RuntimeEnvironment.directory(child(getDataDir(), "app_" + name));
  }
  private static File child(File root, String name) {
    if (name.contains("/") || name.contains("\\") || name.equals(".."))
      throw new IllegalArgumentException("Invalid private filename");
    return new File(root, RuntimeEnvironment.hostName(name));
  }
  public String getPackageCodePath() {
    return getApplicationInfo().sourceDir;
  }
  public String getPackageResourcePath() {
    return getPackageCodePath();
  }
  public FileInputStream openFileInput(String name) throws FileNotFoundException {
    return new FileInputStream(child(getFilesDir(), name));
  }
  public FileOutputStream openFileOutput(String name, int mode) throws FileNotFoundException {
    return new FileOutputStream(child(getFilesDir(), name), (mode & 32768) != 0);
  }
  public boolean deleteFile(String name) {
    return child(getFilesDir(), name).delete();
  }
  public AssetManager getAssets() {
    RuntimeEnvironment.trace("Context.getAssets");
    return RuntimeEnvironment.get().assets;
  }
  public Resources getResources() {
    return RuntimeEnvironment.resources();
  }
  public String getString(int id) {
    return getResources().getString(id);
  }
  public String getString(int id, Object... args) {
    return getResources().getString(id, args);
  }
  public CharSequence getText(int id) {
    return getResources().getText(id);
  }
  public Resources.Theme getTheme() {
    if (theme == null)
      theme = getResources().newTheme();
    return theme;
  }
  public void setTheme(int id) {
    getTheme().applyStyle(id, true);
  }
  public TypedArray obtainStyledAttributes(int[] attrs) {
    return getTheme().obtainStyledAttributes(attrs);
  }
  public TypedArray obtainStyledAttributes(int style, int[] attrs) {
    return getTheme().obtainStyledAttributes(style, attrs);
  }
  public TypedArray obtainStyledAttributes(android.util.AttributeSet set, int[] attrs) {
    return getTheme().obtainStyledAttributes(set, attrs, 0, 0);
  }
  public TypedArray obtainStyledAttributes(
      android.util.AttributeSet set, int[] attrs, int attr, int def) {
    return getTheme().obtainStyledAttributes(set, attrs, attr, def);
  }
  public android.graphics.drawable.Drawable getDrawable(int id) {
    return getResources().getDrawable(id, getTheme());
  }
  private final ContentResolver resolver = new ContentResolver();
  public ContentResolver getContentResolver() {
    return resolver;
  }
  public Context createConfigurationContext(Configuration override) {
    Context parent = this;
    Configuration merged = new Configuration(getResources().getConfiguration());
    merged.updateFrom(override);
    DisplayMetricsHolder holder = new DisplayMetricsHolder(getResources().getDisplayMetrics());
    Resources scoped = new Resources(getAssets(), holder.metrics, merged);
    return new ContextWrapper(parent) {
      public Resources getResources() {
        return scoped;
      }
    };
  }
  private static final class DisplayMetricsHolder {
    final android.util.DisplayMetrics metrics = new android.util.DisplayMetrics();
    DisplayMetricsHolder(android.util.DisplayMetrics source) {
      metrics.setTo(source);
    }
  }
  public SharedPreferences getSharedPreferences(String name, int mode) {
    if (mode != 0 && mode != 4)
      throw new UnsupportedOperationException("SharedPreferences mode");
    File f = child(new File(getDataDir(), "shared_prefs"), name + ".xml");
    return preferences.computeIfAbsent(f.toString(), p -> new XmlPreferences(f.toPath()));
  }
  public synchronized Object getSystemService(String name) {
    RuntimeEnvironment.trace("Context.getSystemService(" + name + ")");
    return services.computeIfAbsent(name, n -> switch (n) {
      case ACTIVITY_SERVICE -> new android.app.ActivityManager();
      case LOCATION_SERVICE -> new android.location.LocationManager();
      case BLUETOOTH_SERVICE -> new android.bluetooth.BluetoothManager(this);
      case "uimode" -> new android.app.UiModeManager();
      case CAPTIONING_SERVICE -> new android.view.accessibility.CaptioningManager(this);
      case LOCALE_SERVICE -> new android.app.LocaleManager(this);
      case "layout_inflater" -> new android.view.LayoutInflater(this);
      case "sensor" -> new android.hardware.SensorManager();
      case WINDOW_SERVICE -> new android.view.WindowManagerImpl(this);
      case AUDIO_SERVICE -> new android.media.AudioManager();
      case MEDIA_ROUTER_SERVICE -> new android.media.MediaRouter();
      case POWER_SERVICE -> new PowerManager();
      case INPUT_METHOD_SERVICE -> new android.view.inputmethod.InputMethodManager();
      case VIBRATOR_SERVICE -> new Vibrator();
      case VIBRATOR_MANAGER_SERVICE -> new VibratorManager(this);
      case TELEPHONY_SERVICE -> new android.telephony.TelephonyManager();
      case CONNECTIVITY_SERVICE -> new android.net.ConnectivityManager();
      case CLIPBOARD_SERVICE -> new ClipboardManager();
      case BATTERY_SERVICE -> new BatteryManager();
      case DISPLAY_SERVICE -> new android.hardware.display.DisplayManager();
      case INPUT_SERVICE -> android.hardware.input.InputManager.getInstance();
      case USER_SERVICE -> new android.os.UserManager();
      case WIFI_SERVICE -> new android.net.wifi.WifiManager();
      case JOB_SCHEDULER_SERVICE -> android.app.job.JobScheduler.ventureLocal(this);
      case STORAGE_SERVICE -> new android.os.storage.StorageManager();
      case ALARM_SERVICE -> new android.app.AlarmManager();
      case NOTIFICATION_SERVICE -> new android.app.NotificationManager();
      default -> null;
    });
  }
  // API 23: the service registered for a manager class, through the name lookup
  // that wrappers and components may override.
  private static final java.util.Map<Class<?>, String> SERVICE_NAMES = java.util.Map.ofEntries(
      java.util.Map.entry(android.app.ActivityManager.class, ACTIVITY_SERVICE),
      java.util.Map.entry(android.view.accessibility.CaptioningManager.class, CAPTIONING_SERVICE),
      java.util.Map.entry(android.app.LocaleManager.class, LOCALE_SERVICE),
      java.util.Map.entry(android.location.LocationManager.class, LOCATION_SERVICE),
      java.util.Map.entry(android.bluetooth.BluetoothManager.class, BLUETOOTH_SERVICE),
      java.util.Map.entry(android.app.UiModeManager.class, UI_MODE_SERVICE),
      java.util.Map.entry(android.view.LayoutInflater.class, LAYOUT_INFLATER_SERVICE),
      java.util.Map.entry(android.hardware.SensorManager.class, SENSOR_SERVICE),
      java.util.Map.entry(android.view.WindowManager.class, WINDOW_SERVICE),
      java.util.Map.entry(android.media.AudioManager.class, AUDIO_SERVICE),
      java.util.Map.entry(android.media.MediaRouter.class, MEDIA_ROUTER_SERVICE),
      java.util.Map.entry(PowerManager.class, POWER_SERVICE),
      java.util.Map.entry(android.view.inputmethod.InputMethodManager.class, INPUT_METHOD_SERVICE),
      java.util.Map.entry(Vibrator.class, VIBRATOR_SERVICE),
      java.util.Map.entry(VibratorManager.class, VIBRATOR_MANAGER_SERVICE),
      java.util.Map.entry(android.telephony.TelephonyManager.class, TELEPHONY_SERVICE),
      java.util.Map.entry(android.net.ConnectivityManager.class, CONNECTIVITY_SERVICE),
      java.util.Map.entry(ClipboardManager.class, CLIPBOARD_SERVICE),
      java.util.Map.entry(BatteryManager.class, BATTERY_SERVICE),
      java.util.Map.entry(android.hardware.display.DisplayManager.class, DISPLAY_SERVICE),
      java.util.Map.entry(android.hardware.input.InputManager.class, INPUT_SERVICE),
      java.util.Map.entry(android.os.UserManager.class, USER_SERVICE),
      java.util.Map.entry(android.net.wifi.WifiManager.class, WIFI_SERVICE),
      java.util.Map.entry(android.app.job.JobScheduler.class, JOB_SCHEDULER_SERVICE),
      java.util.Map.entry(android.os.storage.StorageManager.class, STORAGE_SERVICE),
      java.util.Map.entry(android.app.AlarmManager.class, ALARM_SERVICE),
      java.util.Map.entry(android.app.NotificationManager.class, NOTIFICATION_SERVICE));
  public String getSystemServiceName(Class<?> serviceClass) {
    return SERVICE_NAMES.get(serviceClass);
  }
  public final <T> T getSystemService(Class<T> serviceClass) {
    String name = getSystemServiceName(serviceClass);
    if (name == null) return null;
    Object service = getSystemService(name);
    return serviceClass.isInstance(service) ? serviceClass.cast(service) : null;
  }
  public int checkPermission(String permission, int pid, int uid) {
    // AOSP ContextImpl.checkPermission checks the requested UID, with pid=-1
    // permitted. Only the hosted application's UID has installed grant data.
    if (permission == null) throw new IllegalArgumentException("permission is null");
    if (uid != android.os.Process.myUid()) return PackageManager.PERMISSION_DENIED;
    return getPackageManager().checkPermission(permission, getPackageName());
  }
  public int checkSelfPermission(String permission) {
    return checkPermission(permission, android.os.Process.myPid(), android.os.Process.myUid());
  }
  public int checkCallingOrSelfPermission(String permission) {
    return checkPermission(permission, Binder.getCallingPid(), Binder.getCallingUid());
  }
  public Looper getMainLooper() {
    return Looper.getMainLooper();
  }
  public java.util.concurrent.Executor getMainExecutor() {
    // AOSP ContextImpl / HandlerExecutor: always enqueue, even on the main thread.
    Handler handler = new Handler(getMainLooper());
    return command -> {
      if (!handler.post(Objects.requireNonNull(command)))
        throw new java.util.concurrent.RejectedExecutionException("Main Looper has stopped");
    };
  }
  // ContextImpl.getDisplay: only visual contexts (Activity) are associated with a display.
  public android.view.Display getDisplay() {
    throw new UnsupportedOperationException("Tried to obtain display from a Context not associated with one. "
        + "Only visual Contexts (such as Activity or one created with Context#createWindowContext) or ones created "
        + "with Context#createDisplayContext are associated with displays. Other types of Contexts are typically "
        + "related to background entities and may return an arbitrary display.");
  }
  // ContextImpl.sendBroadcast: receivers registered in this process by any
  // Context whose filter matches run onReceive asynchronously on their Handler
  // (the main thread without one), given the Context that registered them.
  // An explicit broadcast goes only to its component: the manifest receiver, which is
  // instantiated for the delivery and given the application Context, as on Android.
  public void sendBroadcast(Intent intent) {
    ComponentName component = intent.getComponent();
    if (component != null) {
      deliverToManifestReceiver(component, new Intent(intent));
      return;
    }
    for (var r : registrations) {
      for (var filter : r.filters) {
        if (filter.match(getContentResolver(), intent, true, "Context") < 0) continue;
        r.post(intent);
        break; // One delivery per receiver, even when several filters match.
      }
    }
  }
  private void deliverToManifestReceiver(ComponentName component, Intent intent) {
    var model = RuntimeEnvironment.get().manifest;
    android.content.pm.ActivityInfo receiver = null;
    if (component.getPackageName().equals(getPackageName()))
      for (var r : model.receivers) if (r.name.equals(component.getClassName())) receiver = r;
    if (receiver == null || !receiver.enabled) {
      android.util.Log.w("BroadcastQueue", "Background execution not allowed: receiving " + intent + " to " + component.flattenToShortString());
      return;
    }
    Context app = getApplicationContext() != null ? getApplicationContext() : this;
    String name = receiver.name;
    new Handler(getMainLooper()).post(() -> {
      BroadcastReceiver instance;
      try {
        instance = (BroadcastReceiver) Class.forName(name, true, RuntimeEnvironment.get().loader)
            .getDeclaredConstructor().newInstance();
      } catch (ReflectiveOperationException e) {
        throw new RuntimeException("Unable to instantiate receiver " + name + ": " + e, e);
      }
      intent.setExtrasClassLoader(RuntimeEnvironment.get().loader);
      instance.ventureDeliver(app, intent);
    });
  }
  public static final int RECEIVER_VISIBLE_TO_INSTANT_APPS = 0x1, RECEIVER_EXPORTED = 0x2,
      RECEIVER_NOT_EXPORTED = 0x4;
  public Intent registerReceiver(BroadcastReceiver receiver, IntentFilter filter) {
    return registerReceiver(receiver, filter, null, null);
  }
  public Intent registerReceiver(BroadcastReceiver receiver, IntentFilter filter, int flags) {
    return registerReceiver(receiver, filter, null, null, flags);
  }
  // API 26 overloads; the flags only control exposure to other apps (ContextImpl).
  public Intent registerReceiver(
      BroadcastReceiver receiver, IntentFilter filter, String permission, Handler scheduler, int flags) {
    if ((flags & RECEIVER_EXPORTED) != 0 && (flags & RECEIVER_NOT_EXPORTED) != 0)
      throw new IllegalArgumentException(
          "Receiver can't specify both RECEIVER_EXPORTED and RECEIVER_NOT_EXPORTED flag");
    return registerReceiver(receiver, filter, permission, scheduler);
  }
  public Intent registerReceiver(
      BroadcastReceiver receiver, IntentFilter filter, String permission, Handler scheduler) {
    RuntimeEnvironment.trace("Context.registerReceiver");
    Context owner = ventureOuterContext();
    Objects.requireNonNull(filter);
    Registration registration = null;
    if (receiver != null)
      synchronized (registrations) {
        if (scheduler == null) {
          if (receiverMainHandler == null) receiverMainHandler = new Handler(getMainLooper());
          scheduler = receiverMainHandler;
        }
        for (var r : registrations)
          if (r.owner == owner && r.receiver == receiver) { registration = r; break; }
        if (registration == null) {
          registration = new Registration(owner, receiver, scheduler);
          registrations.add(registration);
        } else if (registration.scheduler != scheduler) {
          throw new IllegalStateException("Receiver registered with differing handler");
        }
        registration.filters.add(new IntentFilter(filter));
        registration.trace("register");
      }
    Intent first = null;
    for (int i = 0; i < filter.countActions(); i++) {
      Intent intent = sticky(filter.getAction(i));
      if (intent != null) {
        if (first == null)
          first = intent;
        if (registration != null) registration.post(intent);
      }
    }
    return first;
  }
  private static Intent sticky(String action) {
    return switch (action) {
      case Intent.ACTION_BATTERY_CHANGED ->
        new Intent(action)
            .putExtra("level", 100)
            .putExtra("scale", 100)
            .putExtra("status", 5)
            .putExtra("plugged", 1)
            .putExtra("present", false);
      case Intent.ACTION_HEADSET_PLUG ->
        new Intent(action).putExtra("state", 0).putExtra("microphone", 0).putExtra("name", "");
      case "android.media.VOLUME_CHANGED_ACTION" ->
        new Intent(action)
            .putExtra("android.media.EXTRA_VOLUME_STREAM_TYPE", 3)
            .putExtra("android.media.EXTRA_VOLUME_STREAM_VALUE", 15);
      default -> null;
    };
  }
  // LoadedApk tracks receivers per registering Context.
  public void unregisterReceiver(BroadcastReceiver receiver) {
    Context owner = ventureOuterContext();
    synchronized (registrations) {
      for (var r : registrations) {
        if (r.owner != owner || r.receiver != receiver) continue;
        r.forgotten = true;
        registrations.remove(r);
        r.trace("unregister");
        return;
      }
      throw new IllegalArgumentException("Receiver not registered: " + receiver);
    }
  }
  /** LoadedApk-style owner cleanup after component destruction. */
  public static void ventureRemoveReceiverRegistrations(Context owner) {
    forgetReceivers(Objects.requireNonNull(owner).ventureOuterContext());
  }
  public static void ventureClearReceiverRegistrations() { forgetReceivers(null); }
  private static void forgetReceivers(Context owner) {
    var leaked = new ArrayList<Registration>();
    synchronized (registrations) {
      for (var registration : registrations) {
        if (owner != null && registration.owner != owner) continue;
        registration.forgotten = true;
        leaked.add(registration);
      }
      registrations.removeAll(leaked);
    }
    for (var registration : leaked) {
      registration.trace("unregister");
      android.util.Log.w("Context", "Leaked receiver " + registration.receiver.getClass().getName()
          + " owned by " + registration.owner.getClass().getName() + "; unregistering");
    }
  }
  public void startActivity(Intent intent) {
    AppMain.startActivity(intent);
  }
  public void startActivity(Intent intent, android.os.Bundle options) {
    startActivity(intent);
  }
  public void startActivities(Intent[] intents) {
    for (Intent intent : intents) startActivity(intent);
  }
  public void startActivities(Intent[] intents, android.os.Bundle options) {
    startActivities(intents);
  }
  // A local service runs in this process either way.
  public ComponentName startForegroundService(Intent intent) {
    return startService(intent);
  }
  public final int getColor(int id) {
    return getResources().getColor(id, getTheme());
  }
  public String getOpPackageName() {
    return getPackageName();
  }
  public String getAttributionTag() {
    return null;
  }
  public Context createAttributionContext(String attributionTag) {
    return this;
  }
  public boolean isRestricted() {
    return false;
  }
  // One storage area: the app's credential-protected and device-protected data are the same files.
  public Context createDeviceProtectedStorageContext() {
    return this;
  }
  public boolean isDeviceProtectedStorage() {
    return false;
  }
  public ComponentName startService(Intent intent) {
    return LocalServices.start(this, intent);
  }
  public boolean stopService(Intent intent) {
    return LocalServices.stop(this, intent);
  }
  public boolean bindService(Intent intent, ServiceConnection connection, int flags) {
    Handler handler = new Handler(getMainLooper());
    return LocalServices.bind(this, intent, connection, flags, command -> handler.post(command));
  }
  public boolean bindService(Intent intent, int flags, java.util.concurrent.Executor executor, ServiceConnection connection) {
    return LocalServices.bind(this, intent, connection, flags, executor);
  }
  public void unbindService(ServiceConnection connection) {
    LocalServices.unbind(this, connection);
  }
}
