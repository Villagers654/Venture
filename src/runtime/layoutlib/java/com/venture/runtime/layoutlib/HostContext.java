package com.venture.runtime.layoutlib;

import android.content.AttributionSource;
import android.content.BroadcastReceiver;
import android.content.ClipboardManager;
import android.content.ComponentName;
import android.content.ContentResolver;
import android.content.Context;
import android.content.ContextParams;
import android.content.ContextWrapper;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.ServiceConnection;
import android.content.SharedPreferences;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.database.DatabaseErrorHandler;
import android.database.sqlite.SQLiteDatabase;
import android.os.BatteryManager;
import android.os.Binder;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;
import android.os.UserHandle;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.view.Display;
import com.venture.runtime.AppMain;
import com.venture.runtime.LocalServices;
import com.venture.runtime.RuntimeEnvironment;
import com.venture.runtime.XmlPreferences;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.Executor;

/**
 * The base context of a 2D process's application, activities and services, in
 * place of AOSP's ContextImpl: files, preferences, broadcasts, services and the
 * system services come from Venture's host services, as the classic framework's
 * Context serves them; resources come from the app's APK over layoutlib's
 * libandroidfw (docs/design/layoutlib.md).
 */
public class HostContext extends ContextStubs {
  private static final Map<String, SharedPreferences> preferences = new ConcurrentHashMap<>();
  private static final boolean TRACE_RECEIVERS = System.getenv("VENTURE_TRACE_RECEIVERS") != null;
  private static long nextReceiverId;
  private static Handler receiverMainHandler;

  private static final class Registration {
    final Context owner;
    final BroadcastReceiver receiver;
    final Handler scheduler;
    final long id;
    final List<IntentFilter> filters = new CopyOnWriteArrayList<>();
    volatile boolean forgotten;
    Registration(Context owner, BroadcastReceiver receiver, Handler scheduler) {
      this.owner = owner;
      this.receiver = receiver;
      this.scheduler = scheduler;
      id = ++nextReceiverId;  // registrations lock
    }
    void trace(String event) {
      if (TRACE_RECEIVERS) android.util.Log.i("BroadcastReceiver", event + " id=" + id);
    }
    void post(Intent intent) {
      Intent copy = new Intent(intent);
      scheduler.post(() -> {
        if (forgotten) { trace("cancel"); return; }
        trace("deliver");
        android.content.VentureReceivers.deliver(receiver, owner, copy);
      });
    }
  }
  private static final List<Registration> registrations = new CopyOnWriteArrayList<>();

  private final Context outer;
  private final Display display;
  private final Map<String, Object> services = new HashMap<>();
  private Resources.Theme theme;
  private final ContentResolver resolver = new ContentResolver();

  public HostContext() { this(null, null); }

  /** A component's base context: receivers and connections belong to outer; display is the window's. */
  public HostContext(Context outer, Display display) {
    this.outer = outer;
    this.display = display;
  }

  /** The Activity or Service whose receivers and connections this context's calls belong to. */
  public Context outerContext() { return outer == null ? this : outer; }

  public static Context outerOf(Context context) {
    for (Context c = context; c != null; ) {
      if (c instanceof HostContext host) return host.outerContext();
      if (!(c instanceof ContextWrapper wrapper)) break;
      c = wrapper.getBaseContext();
    }
    return context;
  }

  // Identity and code.
  @Override public String getPackageName() { return RuntimeEnvironment.get().manifest.info.packageName; }
  @Override public String getBasePackageName() { return getPackageName(); }
  @Override public String getOpPackageName() { return getPackageName(); }
  @Override public Context getApplicationContext() {
    var application = RuntimeEnvironment.get().application;
    return application == null ? this : application;
  }
  @Override public ClassLoader getClassLoader() { return RuntimeEnvironment.get().loader; }
  @Override public ApplicationInfo getApplicationInfo() { return RuntimeEnvironment.get().manifest.info.applicationInfo; }
  @Override public PackageManager getPackageManager() { return RuntimeEnvironment.get().packageManager; }
  @Override public String getPackageCodePath() { return getApplicationInfo().sourceDir; }
  @Override public String getPackageResourcePath() { return getPackageCodePath(); }
  @Override public Looper getMainLooper() { return Looper.getMainLooper(); }
  @Override public Executor getMainExecutor() {
    Handler handler = new Handler(getMainLooper());
    return command -> {
      if (!handler.post(Objects.requireNonNull(command)))
        throw new java.util.concurrent.RejectedExecutionException("Main Looper has stopped");
    };
  }
  @Override public UserHandle getUser() { return android.os.Process.myUserHandle(); }
  @Override public int getUserId() { return 0; }
  @Override public boolean isRestricted() { return false; }
  @Override public boolean canLoadUnsafeResources() { return true; }

  // Resources.
  @Override public AssetManager getAssets() { return getResources().getAssets(); }
  @Override public Resources getResources() { return RuntimeEnvironment.resources(); }
  @Override public Resources.Theme getTheme() {
    if (theme == null) {
      theme = getResources().newTheme();
      int id = getApplicationInfo().theme;
      theme.applyStyle(id != 0 ? id : Resources.selectDefaultTheme(0, getApplicationInfo().targetSdkVersion), true);
    }
    return theme;
  }
  @Override public void setTheme(int id) { getTheme().applyStyle(id, true); }
  @Override public Context createConfigurationContext(Configuration override) {
    Configuration merged = new Configuration(getResources().getConfiguration());
    merged.updateFrom(override);
    android.util.DisplayMetrics metrics = new android.util.DisplayMetrics();
    metrics.setTo(getResources().getDisplayMetrics());
    // Its own AssetManager over the same APKs, as ResourcesManager makes one per
    // configuration: an AssetManager holds one configuration, and resources
    // sharing it would resolve with whichever configuration was set last.
    AssetManager assets = new AssetManager();
    assets.setApkAssets(getResources().getAssets().getApkAssets(), false);
    Resources scoped = new Resources(assets, metrics, merged);
    return new ContextWrapper(outerContext()) {
      @Override public Resources getResources() { return scoped; }
      @Override public AssetManager getAssets() { return scoped.getAssets(); }
    };
  }
  @Override public Context createDisplayContext(Display display) {
    return new HostContext(outer, Objects.requireNonNull(display));
  }
  @Override public Context createWindowContext(int type, android.os.Bundle options) {
    return new HostContext(outer, display != null ? display : Layoutlib.display());
  }
  @Override public Context createWindowContext(Display display, int type, android.os.Bundle options) {
    return new HostContext(outer, display);
  }
  @Override public Display getDisplay() {
    if (display == null)
      throw new UnsupportedOperationException("Tried to obtain display from a Context not associated with one. "
          + "Only visual Contexts (such as Activity or one created with Context#createWindowContext) or ones "
          + "created with Context#createDisplayContext are associated with displays.");
    return display;
  }
  @Override public Display getDisplayNoVerify() { return display != null ? display : Layoutlib.display(); }
  @Override public int getDisplayId() { return 0; }
  @Override public int getAssociatedDisplayId() { return display != null ? 0 : -1; }
  @Override public void updateDisplay(int displayId) {}
  @Override public android.view.DisplayAdjustments getDisplayAdjustments(int displayId) {
    return new android.view.DisplayAdjustments(getResources().getConfiguration());
  }
  @Override public boolean isUiContext() { return display != null; }

  // Files.
  private static File child(File root, String name) {
    if (name.contains("/") || name.contains("\\") || name.equals(".."))
      throw new IllegalArgumentException("Invalid private filename");
    return new File(root, RuntimeEnvironment.hostName(name));
  }
  @Override public File getDataDir() { return RuntimeEnvironment.directory(RuntimeEnvironment.get().data); }
  @Override public File getFilesDir() { return RuntimeEnvironment.directory(new File(RuntimeEnvironment.get().data, "files")); }
  @Override public File getCacheDir() { return RuntimeEnvironment.directory(RuntimeEnvironment.get().cache); }
  @Override public File getCodeCacheDir() { return RuntimeEnvironment.directory(new File(getDataDir(), "code_cache")); }
  @Override public File getNoBackupFilesDir() { return RuntimeEnvironment.directory(new File(getDataDir(), "no_backup")); }
  @Override public File getObbDir() { return RuntimeEnvironment.get().obb; }
  @Override public File[] getObbDirs() { return new File[] {getObbDir()}; }
  @Override public File getExternalFilesDir(String type) {
    File root = RuntimeEnvironment.directory(RuntimeEnvironment.get().external);
    return type == null ? root : RuntimeEnvironment.directory(child(root, type));
  }
  @Override public File[] getExternalFilesDirs(String type) { return new File[] {getExternalFilesDir(type)}; }
  @Override public File getExternalCacheDir() {
    return RuntimeEnvironment.directory(new File(RuntimeEnvironment.get().data, "external-data/cache"));
  }
  @Override public File[] getExternalCacheDirs() { return new File[] {getExternalCacheDir()}; }
  @Override public File[] getExternalMediaDirs() { return new File[0]; }
  @Override public File getFileStreamPath(String name) { return child(getFilesDir(), name); }
  @Override public String[] fileList() {
    String[] names = getFilesDir().list();
    return names != null ? names : new String[0];
  }
  @Override public File getDir(String name, int mode) { return RuntimeEnvironment.directory(child(getDataDir(), "app_" + name)); }
  @Override public FileInputStream openFileInput(String name) throws FileNotFoundException {
    return new FileInputStream(child(getFilesDir(), name));
  }
  @Override public FileOutputStream openFileOutput(String name, int mode) throws FileNotFoundException {
    return new FileOutputStream(child(getFilesDir(), name), (mode & MODE_APPEND) != 0);
  }
  @Override public boolean deleteFile(String name) { return child(getFilesDir(), name).delete(); }
  @Override public File getDatabasePath(String name) {
    if (new File(name).isAbsolute() || name.startsWith("/")) return new File(name);
    return child(RuntimeEnvironment.directory(new File(getDataDir(), "databases")), name);
  }
  @Override public SQLiteDatabase openOrCreateDatabase(String name, int mode, SQLiteDatabase.CursorFactory factory) {
    return SQLiteDatabase.openOrCreateDatabase(getDatabasePath(name), factory);
  }
  @Override public SQLiteDatabase openOrCreateDatabase(String name, int mode, SQLiteDatabase.CursorFactory factory,
                                                       DatabaseErrorHandler errorHandler) {
    return openOrCreateDatabase(name, mode, factory);
  }
  @Override public boolean deleteDatabase(String name) { return SQLiteDatabase.deleteDatabase(getDatabasePath(name)); }
  @Override public String[] databaseList() {
    String[] names = new File(getDataDir(), "databases").list();
    return names != null ? names : new String[0];
  }
  @Override public Context createDeviceProtectedStorageContext() { return this; }
  @Override public Context createCredentialProtectedStorageContext() { return this; }
  @Override public boolean isDeviceProtectedStorage() { return false; }
  @Override public boolean isCredentialProtectedStorage() { return false; }

  // Preferences.
  @Override public File getSharedPreferencesPath(String name) {
    return child(new File(getDataDir(), "shared_prefs"), name + ".xml");
  }
  @Override public SharedPreferences getSharedPreferences(String name, int mode) {
    return getSharedPreferences(getSharedPreferencesPath(name), mode);
  }
  @Override public SharedPreferences getSharedPreferences(File file, int mode) {
    if (mode != MODE_PRIVATE && mode != MODE_MULTI_PROCESS)
      throw new UnsupportedOperationException("SharedPreferences mode");
    return preferences.computeIfAbsent(file.toString(), p -> new XmlPreferences(file.toPath()));
  }
  @Override public boolean deleteSharedPreferences(String name) {
    File file = getSharedPreferencesPath(name);
    preferences.remove(file.toString());
    return !file.exists() || file.delete();
  }
  @Override public void reloadSharedPreferences() {}

  // System services.
  @Override public synchronized Object getSystemService(String name) {
    RuntimeEnvironment.trace("Context.getSystemService(" + name + ")");
    // A window, its inflater and its input method follow this context's display and theme.
    switch (name) {
      case WINDOW_SERVICE: return new android.view.WindowManagerImpl(this);
      case LAYOUT_INFLATER_SERVICE: return new com.android.internal.policy.PhoneLayoutInflater(this);
      default: break;
    }
    return services.computeIfAbsent(name, n -> switch (n) {
      case ACTIVITY_SERVICE -> new android.app.ActivityManager();
      case LOCATION_SERVICE -> new android.location.LocationManager();
      case BLUETOOTH_SERVICE -> new android.bluetooth.BluetoothManager(this);
      case UI_MODE_SERVICE -> new android.app.UiModeManager();
      case LOCALE_SERVICE -> new android.app.LocaleManager(this);
      case GAME_SERVICE -> new android.app.GameManager(this);
      case SENSOR_SERVICE -> new android.hardware.SensorManager();
      case CAMERA_SERVICE -> new android.hardware.camera2.CameraManager();
      case AUDIO_SERVICE -> new android.media.AudioManager();
      case MEDIA_ROUTER_SERVICE -> new android.media.MediaRouter();
      case POWER_SERVICE -> new PowerManager();
      case INPUT_METHOD_SERVICE -> android.view.inputmethod.InputMethodManager.forContext(this);
      case ACCESSIBILITY_SERVICE -> android.view.accessibility.AccessibilityManager.getInstance(this);
      case TEXT_SERVICES_MANAGER_SERVICE -> android.view.textservice.TextServicesManager.getInstance();
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
      case DOWNLOAD_SERVICE -> android.app.DownloadManager.ventureLocal(this);
      case NSD_SERVICE -> new android.net.nsd.NsdManager();
      case KEYGUARD_SERVICE -> new android.app.KeyguardManager();
      case USAGE_STATS_SERVICE -> new android.app.usage.UsageStatsManager();
      case DEVICE_POLICY_SERVICE -> new android.app.admin.DevicePolicyManager();
      case BIOMETRIC_SERVICE -> new android.hardware.biometrics.BiometricManager();
      case USB_SERVICE -> new android.hardware.usb.UsbManager(this);
      case MIDI_SERVICE -> new android.media.midi.MidiManager();
      default -> null;
    });
  }
  private static final Map<Class<?>, String> SERVICE_NAMES = Map.ofEntries(
      Map.entry(android.app.ActivityManager.class, ACTIVITY_SERVICE),
      Map.entry(android.app.LocaleManager.class, LOCALE_SERVICE),
      Map.entry(android.app.GameManager.class, GAME_SERVICE),
      Map.entry(android.location.LocationManager.class, LOCATION_SERVICE),
      Map.entry(android.bluetooth.BluetoothManager.class, BLUETOOTH_SERVICE),
      Map.entry(android.app.UiModeManager.class, UI_MODE_SERVICE),
      Map.entry(android.view.LayoutInflater.class, LAYOUT_INFLATER_SERVICE),
      Map.entry(android.hardware.SensorManager.class, SENSOR_SERVICE),
      Map.entry(android.hardware.camera2.CameraManager.class, CAMERA_SERVICE),
      Map.entry(android.view.WindowManager.class, WINDOW_SERVICE),
      Map.entry(android.media.AudioManager.class, AUDIO_SERVICE),
      Map.entry(android.media.MediaRouter.class, MEDIA_ROUTER_SERVICE),
      Map.entry(PowerManager.class, POWER_SERVICE),
      Map.entry(android.view.inputmethod.InputMethodManager.class, INPUT_METHOD_SERVICE),
      Map.entry(android.view.accessibility.AccessibilityManager.class, ACCESSIBILITY_SERVICE),
      Map.entry(android.view.textservice.TextServicesManager.class, TEXT_SERVICES_MANAGER_SERVICE),
      Map.entry(Vibrator.class, VIBRATOR_SERVICE),
      Map.entry(VibratorManager.class, VIBRATOR_MANAGER_SERVICE),
      Map.entry(android.telephony.TelephonyManager.class, TELEPHONY_SERVICE),
      Map.entry(android.net.ConnectivityManager.class, CONNECTIVITY_SERVICE),
      Map.entry(ClipboardManager.class, CLIPBOARD_SERVICE),
      Map.entry(BatteryManager.class, BATTERY_SERVICE),
      Map.entry(android.hardware.display.DisplayManager.class, DISPLAY_SERVICE),
      Map.entry(android.hardware.input.InputManager.class, INPUT_SERVICE),
      Map.entry(android.os.UserManager.class, USER_SERVICE),
      Map.entry(android.net.wifi.WifiManager.class, WIFI_SERVICE),
      Map.entry(android.app.job.JobScheduler.class, JOB_SCHEDULER_SERVICE),
      Map.entry(android.os.storage.StorageManager.class, STORAGE_SERVICE),
      Map.entry(android.app.AlarmManager.class, ALARM_SERVICE),
      Map.entry(android.app.NotificationManager.class, NOTIFICATION_SERVICE),
      Map.entry(android.app.DownloadManager.class, DOWNLOAD_SERVICE),
      Map.entry(android.net.nsd.NsdManager.class, NSD_SERVICE),
      Map.entry(android.app.KeyguardManager.class, KEYGUARD_SERVICE),
      Map.entry(android.app.usage.UsageStatsManager.class, USAGE_STATS_SERVICE),
      Map.entry(android.app.admin.DevicePolicyManager.class, DEVICE_POLICY_SERVICE),
      Map.entry(android.hardware.biometrics.BiometricManager.class, BIOMETRIC_SERVICE),
      Map.entry(android.hardware.usb.UsbManager.class, USB_SERVICE),
      Map.entry(android.media.midi.MidiManager.class, MIDI_SERVICE));
  @Override public String getSystemServiceName(Class<?> serviceClass) { return SERVICE_NAMES.get(serviceClass); }

  // Permissions: only the hosted application's uid has grants.
  @Override public int checkPermission(String permission, int pid, int uid) {
    if (permission == null) throw new IllegalArgumentException("permission is null");
    if (uid != android.os.Process.myUid()) return PackageManager.PERMISSION_DENIED;
    return getPackageManager().checkPermission(permission, getPackageName());
  }
  @Override public int checkPermission(String permission, int pid, int uid, android.os.IBinder token) {
    return checkPermission(permission, pid, uid);
  }
  @Override public int checkSelfPermission(String permission) {
    return checkPermission(permission, android.os.Process.myPid(), android.os.Process.myUid());
  }
  @Override public int checkCallingPermission(String permission) {
    return checkPermission(permission, Binder.getCallingPid(), Binder.getCallingUid());
  }
  @Override public int checkCallingOrSelfPermission(String permission) {
    return checkPermission(permission, Binder.getCallingPid(), Binder.getCallingUid());
  }
  @Override public void enforcePermission(String permission, int pid, int uid, String message) {
    if (checkPermission(permission, pid, uid) != PackageManager.PERMISSION_GRANTED)
      throw new SecurityException((message != null ? message + ": " : "") + "requires " + permission);
  }
  @Override public void enforceCallingPermission(String permission, String message) {
    enforcePermission(permission, Binder.getCallingPid(), Binder.getCallingUid(), message);
  }
  @Override public void enforceCallingOrSelfPermission(String permission, String message) {
    enforcePermission(permission, Binder.getCallingPid(), Binder.getCallingUid(), message);
  }
  @Override public int checkUriPermission(android.net.Uri uri, int pid, int uid, int modeFlags) {
    return PackageManager.PERMISSION_GRANTED;
  }
  @Override public int checkCallingUriPermission(android.net.Uri uri, int modeFlags) { return PackageManager.PERMISSION_GRANTED; }
  @Override public int checkCallingOrSelfUriPermission(android.net.Uri uri, int modeFlags) {
    return PackageManager.PERMISSION_GRANTED;
  }
  @Override public void grantUriPermission(String toPackage, android.net.Uri uri, int modeFlags) {}
  @Override public void revokeUriPermission(android.net.Uri uri, int modeFlags) {}
  @Override public void revokeUriPermission(String toPackage, android.net.Uri uri, int modeFlags) {}

  @Override public ContentResolver getContentResolver() { return resolver; }

  // Broadcasts: receivers registered in this process whose filter matches run
  // onReceive on their Handler with the context that registered them; an
  // explicit broadcast goes to its manifest receiver (ContextImpl, LoadedApk).
  @Override public void sendBroadcast(Intent intent) {
    ComponentName component = intent.getComponent();
    if (component != null) {
      deliverToManifestReceiver(component, new Intent(intent));
      return;
    }
    for (var r : registrations) {
      for (var filter : r.filters) {
        if (filter.match(getContentResolver(), intent, true, "Context") < 0) continue;
        r.post(intent);
        break;
      }
    }
  }
  @Override public void sendBroadcast(Intent intent, String receiverPermission) { sendBroadcast(intent); }
  @Override public void sendBroadcast(Intent intent, String receiverPermission, int appOp) { sendBroadcast(intent); }
  @Override public void sendBroadcastAsUser(Intent intent, UserHandle user) { sendBroadcast(intent); }
  @Override public void sendBroadcastAsUser(Intent intent, UserHandle user, String receiverPermission) {
    sendBroadcast(intent);
  }
  private void deliverToManifestReceiver(ComponentName component, Intent intent) {
    var model = RuntimeEnvironment.get().manifest;
    android.content.pm.ActivityInfo receiver = null;
    if (component.getPackageName().equals(getPackageName()))
      for (var r : model.receivers) if (r.name.equals(component.getClassName())) receiver = r;
    if (receiver == null || !receiver.enabled) {
      android.util.Log.w("BroadcastQueue", "Background execution not allowed: receiving " + intent + " to "
          + component.flattenToShortString());
      return;
    }
    Context app = getApplicationContext();
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
      android.content.VentureReceivers.deliver(instance, app, intent);
    });
  }
  @Override public Intent registerReceiver(BroadcastReceiver receiver, IntentFilter filter) {
    return registerReceiver(receiver, filter, null, null);
  }
  @Override public Intent registerReceiver(BroadcastReceiver receiver, IntentFilter filter, int flags) {
    return registerReceiver(receiver, filter, null, null, flags);
  }
  @Override public Intent registerReceiver(BroadcastReceiver receiver, IntentFilter filter, String permission,
                                           Handler scheduler, int flags) {
    if ((flags & RECEIVER_EXPORTED) != 0 && (flags & RECEIVER_NOT_EXPORTED) != 0)
      throw new IllegalArgumentException("Receiver can't specify both RECEIVER_EXPORTED and RECEIVER_NOT_EXPORTED flag");
    return registerReceiver(receiver, filter, permission, scheduler);
  }
  @Override public Intent registerReceiver(BroadcastReceiver receiver, IntentFilter filter, String permission,
                                           Handler scheduler) {
    Context owner = outerContext();
    Objects.requireNonNull(filter);
    Registration registration = null;
    if (receiver != null)
      synchronized (registrations) {
        if (scheduler == null) {
          if (receiverMainHandler == null) receiverMainHandler = new Handler(Looper.getMainLooper());
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
        if (first == null) first = intent;
        if (registration != null) registration.post(intent);
      }
    }
    return first;
  }
  @Override public Intent registerReceiverAsUser(BroadcastReceiver receiver, UserHandle user, IntentFilter filter,
                                                 String permission, Handler scheduler) {
    return registerReceiver(receiver, filter, permission, scheduler);
  }
  @Override public Intent registerReceiverAsUser(BroadcastReceiver receiver, UserHandle user, IntentFilter filter,
                                                 String permission, Handler scheduler, int flags) {
    return registerReceiver(receiver, filter, permission, scheduler, flags);
  }
  private static Intent sticky(String action) {
    return switch (action) {
      case Intent.ACTION_BATTERY_CHANGED -> new Intent(action).putExtra("level", 100).putExtra("scale", 100)
          .putExtra("status", 5).putExtra("health", 2).putExtra("plugged", 1).putExtra("present", true)
          .putExtra("temperature", 250).putExtra("voltage", 4200).putExtra("technology", "Li-ion")
          .putExtra("battery_low", false);
      case Intent.ACTION_HEADSET_PLUG -> new Intent(action).putExtra("state", 0).putExtra("microphone", 0).putExtra("name", "");
      case "android.media.VOLUME_CHANGED_ACTION" -> new Intent(action)
          .putExtra("android.media.EXTRA_VOLUME_STREAM_TYPE", 3).putExtra("android.media.EXTRA_VOLUME_STREAM_VALUE", 15);
      default -> null;
    };
  }
  @Override public void unregisterReceiver(BroadcastReceiver receiver) {
    Context owner = outerContext();
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
  /** LoadedApk-style cleanup after a component is destroyed; null forgets every registration. */
  public static void forgetReceivers(Context owner) {
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

  // Activities and services.
  @Override public void startActivity(Intent intent) { AppMain.startActivity(intent); }
  @Override public void startActivity(Intent intent, android.os.Bundle options) { startActivity(intent); }
  @Override public void startActivities(Intent[] intents) { for (Intent intent : intents) startActivity(intent); }
  @Override public void startActivities(Intent[] intents, android.os.Bundle options) { startActivities(intents); }
  @Override public ComponentName startService(Intent intent) { return LocalServices.start(this, intent); }
  @Override public ComponentName startForegroundService(Intent intent) { return startService(intent); }
  @Override public boolean stopService(Intent intent) { return LocalServices.stop(this, intent); }
  @Override public boolean bindService(Intent intent, ServiceConnection connection, int flags) {
    Handler handler = new Handler(getMainLooper());
    return LocalServices.bind(this, intent, connection, flags, handler::post);
  }
  @Override public boolean bindService(Intent intent, int flags, Executor executor, ServiceConnection connection) {
    return LocalServices.bind(this, intent, connection, flags, executor);
  }
  @Override public void unbindService(ServiceConnection connection) { LocalServices.unbind(this, connection); }

  // Packages and attribution.
  @Override public Context createPackageContext(String packageName, int flags) throws PackageManager.NameNotFoundException {
    if (packageName != null && packageName.equals(getPackageName())) return outerContext();
    if ("com.oculus.horizon".equals(packageName))
      return new ContextWrapper(outerContext()) {
        @Override public String getPackageName() { return packageName; }
      };
    throw new PackageManager.NameNotFoundException(packageName);
  }
  @Override public Context createApplicationContext(ApplicationInfo application, int flags)
      throws PackageManager.NameNotFoundException {
    return createPackageContext(application.packageName, flags);
  }
  private volatile AttributionSource attributionSource;
  @Override public AttributionSource getAttributionSource() {
    AttributionSource source = attributionSource;
    if (source == null)
      attributionSource = source = new AttributionSource.Builder(android.os.Process.myUid())
          .setPackageName(getOpPackageName()).setAttributionTag(getAttributionTag()).build();
    return source;
  }
  @Override public Context createContext(ContextParams contextParams) {
    Objects.requireNonNull(contextParams);
    return new ContextWrapper(outerContext()) {
      @Override public String getAttributionTag() { return contextParams.getAttributionTag(); }
      @Override public ContextParams getParams() { return contextParams; }
    };
  }
  @Override public Context createAttributionContext(String attributionTag) {
    return createContext(new ContextParams.Builder().setAttributionTag(attributionTag).build());
  }

  // The rest of what ContextImpl overrides (AOSP's Context throws "Not
  // implemented" for these): one device, one user, no system server.
  @Override public int getDeviceId() { return 0; }  // Context.DEVICE_ID_DEFAULT
  @Override public void updateDeviceId(int deviceId) {}
  @Override public void registerDeviceIdChangeListener(Executor executor, java.util.function.IntConsumer listener) {}
  @Override public void unregisterDeviceIdChangeListener(java.util.function.IntConsumer listener) {}
  @Override public Context createDeviceContext(int deviceId) { return outerContext(); }
  @Override public Context createTokenContext(IBinder token, Display display) { return createWindowContext(display, 0, null); }
  @Override public boolean isConfigurationContext() { return display != null; }
  @Override public Handler getMainThreadHandler() { return new Handler(getMainLooper()); }
  @Override public IBinder getActivityToken() { return null; }
  @Override public IBinder getProcessToken() { return null; }
  @Override public IBinder getWindowContextToken() { return null; }
  @Override public android.app.IApplicationThread getIApplicationThread() { return null; }
  @Override public File getCrateDir(String crateId) { return child(new File(getDataDir(), "crates"), crateId); }
  @Override public boolean shouldShowRequestPermissionRationale(String permission) { return false; }
  @Override public int getPermissionRequestState(String permission) { return 0; }
  @Override public void closeSystemDialogs() {}
  @Override public java.util.List<IntentFilter> getRegisteredIntentFilters(BroadcastReceiver receiver) {
    return java.util.List.of();
  }
  @Override public Intent registerReceiverForAllUsers(BroadcastReceiver receiver, IntentFilter filter,
                                                      String permission, Handler scheduler) {
    return registerReceiver(receiver, filter, permission, scheduler);
  }
  @Override public Intent registerReceiverForAllUsers(BroadcastReceiver receiver, IntentFilter filter,
                                                      String permission, Handler scheduler, int flags) {
    return registerReceiver(receiver, filter, permission, scheduler, flags);
  }
  @Override public void sendBroadcast(Intent intent, String receiverPermission, android.os.Bundle options) {
    sendBroadcast(intent);
  }
  @Override public void sendBroadcastMultiplePermissions(Intent intent, String[] receiverPermissions) {
    sendBroadcast(intent);
  }
  @Override public void sendBroadcastMultiplePermissions(Intent intent, String[] receiverPermissions,
                                                         android.os.Bundle options) {
    sendBroadcast(intent);
  }
  @Override public void sendStickyBroadcast(Intent intent, android.os.Bundle options) { sendStickyBroadcast(intent); }
  @Override public void startActivityAsUser(Intent intent, UserHandle user) { startActivity(intent); }
  @Override public void startActivityAsUser(Intent intent, android.os.Bundle options, UserHandle user) {
    startActivity(intent, options);
  }
  @Override public int startActivitiesAsUser(Intent[] intents, android.os.Bundle options, UserHandle user) {
    startActivities(intents, options);
    return 0;  // ActivityManager.START_SUCCESS
  }
  @Override public boolean bindService(Intent intent, ServiceConnection connection, BindServiceFlags flags) {
    return bindService(intent, connection, (int) flags.getValue());
  }
  @Override public boolean bindService(Intent intent, BindServiceFlags flags, Executor executor,
                                       ServiceConnection connection) {
    return bindService(intent, (int) flags.getValue(), executor, connection);
  }
  @Override public boolean bindServiceAsUser(Intent intent, ServiceConnection connection, int flags, UserHandle user) {
    return bindService(intent, connection, flags);
  }
  @Override public boolean bindServiceAsUser(Intent intent, ServiceConnection connection, int flags, Handler handler,
                                             UserHandle user) {
    return LocalServices.bind(this, intent, connection, flags, handler::post);
  }
  @Override public boolean bindIsolatedService(Intent intent, int flags, String instanceName, Executor executor,
                                               ServiceConnection connection) {
    return bindService(intent, flags, executor, connection);
  }
}
