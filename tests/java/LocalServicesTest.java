import android.app.*;
import android.content.*;
import android.content.pm.ServiceInfo;
import android.os.*;
import com.venture.runtime.*;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicReference;

/** Real main Looper and application class loader, with no native library. */
public final class LocalServicesTest {
  private static int checks;
  private static final List<String> events = new CopyOnWriteArrayList<>();
  private static final List<String> leaks = new CopyOnWriteArrayList<>();
  private static final AtomicReference<Throwable> failure = new AtomicReference<>();
  private static Handler main;
  private static void check(boolean condition, String message) {
    checks++;
    if (!condition) throw new AssertionError(message + ": " + events);
  }
  private static void event(String event) {
    if (!Looper.getMainLooper().isCurrentThread()) throw new AssertionError("Callback off main: " + event);
    events.add(event);
  }
  public static class Worker extends Service {
    final IBinder binder = new Binder();
    public void onCreate() { event("create"); }
    public IBinder onBind(Intent intent) { event("bind:" + intent.getAction()); return binder; }
    public boolean onUnbind(Intent intent) { event("unbind"); return true; }
    public void onRebind(Intent intent) { event("rebind"); }
    public int onStartCommand(Intent intent, int flags, int id) { event("start:" + id); return START_NOT_STICKY; }
    public void onDestroy() { event("destroy"); }
  }
  public static class NullWorker extends Worker {
    public IBinder onBind(Intent intent) { event("null-bind"); return null; }
  }
  public static class LeakedWorker extends Worker {}
  public static class SecondaryActivity extends Activity {
    protected void onDestroy() { event("activity-destroy"); }
  }
  public static class ExplicitCleanupActivity extends SecondaryActivity {
    ServiceConnection connection;
    BroadcastReceiver receiver;
    protected void onDestroy() {
      unbindService(connection); unregisterReceiver(receiver);
      super.onDestroy();
    }
  }
  public static class ShutdownWorker extends Worker {
    public boolean onUnbind(Intent intent) { event("shutdown-outgoing-unbind"); return true; }
    public void onDestroy() { event("shutdown-outgoing-destroy"); }
  }
  public static class ShutdownClientWorker extends Worker {
    public boolean onUnbind(Intent intent) { event("shutdown-incoming-unbind"); return true; }
    public void onDestroy() { event("shutdown-incoming-destroy"); }
  }
  public static class ShutdownOwnerService extends Service {
    public void onCreate() {
      bindService(new Intent(this, ShutdownWorker.class), new Client("shutdown-outgoing"), BIND_AUTO_CREATE);
    }
    public IBinder onBind(Intent intent) { return new Binder(); }
    public void onDestroy() { event("shutdown-owner-destroy"); }
  }
  public static class FilteredWorker extends Worker {}
  public static class SwitchingWorker extends Worker {
    public IBinder onBind(Intent intent) {
      event("bind:" + intent.getAction());
      if ("test.NULL".equals(intent.getAction())) return null;
      return "test.CHANGED".equals(intent.getAction()) ? new Binder() : binder;
    }
  }
  private static volatile OwnerService ownerService;
  public static class OwnerService extends Service {
    public void onCreate() { ownerService = this; }
    public IBinder onBind(Intent intent) { return new Binder(); }
  }
  private static class Client implements ServiceConnection {
    final String name;
    volatile IBinder binder;
    Client(String name) { this.name = name; }
    public void onServiceConnected(ComponentName component, IBinder value) {
      binder = value; event("connected:" + name);
    }
    public void onServiceDisconnected(ComponentName component) { event("disconnected:" + name); }
    public void onNullBinding(ComponentName component) { event("null:" + name); }
  }
  private static void drain() throws Exception {
    // Lifecycle callbacks may enqueue connection callbacks behind the first barrier.
    for (int i = 0; i < 2; i++) {
      var done = new CountDownLatch(1);
      main.post(done::countDown);
      if (!done.await(3, TimeUnit.SECONDS)) throw new AssertionError("Main queue did not drain");
    }
  }
  private static ServiceInfo declare(Class<?> type, String process) {
    var env = RuntimeEnvironment.get();
    var info = new ServiceInfo();
    info.name = type.getName(); info.packageName = "test.game";
    info.applicationInfo = env.manifest.info.applicationInfo;
    info.processName = process;
    env.manifest.services.add(info);
    return info;
  }
  private static BinaryResources.Node node(String tag, String... attributes) throws Exception {
    var constructor = BinaryResources.Node.class.getDeclaredConstructor(String.class);
    constructor.setAccessible(true);
    var value = constructor.newInstance(tag);
    for (int i = 0; i < attributes.length; i += 2)
      value.attributes.put(attributes[i], new BinaryResources.Value(3, 0, attributes[i + 1]));
    return value;
  }
  private static void wrapperOwnership() throws Exception {
    events.clear();
    var activity = new Activity();
    activity.ventureAttach(RuntimeEnvironment.get().application); // AppMain's production attach hook.
    var wrapper = new ContextWrapper(activity);
    var intent = new Intent(activity, Worker.class).setAction("test.WRAPPER");
    var client = new Client("wrapper");
    check(wrapper.bindService(intent, client, Context.BIND_AUTO_CREATE), "wrapper bind delegates to activity base");
    drain(); activity.unbindService(client); drain();
    check(events.contains("unbind") && events.contains("destroy"), "activity unbinds wrapper connection and destroys the service");
    events.clear();
    check(wrapper.bindService(intent, Context.BIND_AUTO_CREATE, command -> main.post(command), client), "wrapper executor overload delegates owner");
    drain(); activity.unbindService(client); drain();
    check(events.contains("destroy"), "activity unbinds executor-overload wrapper connection");
    events.clear();
    check(wrapper.startService(intent).equals(intent.getComponent()), "wrapper start delegates"); drain();
    check(activity.stopService(intent), "base activity can stop wrapper-started service"); drain();
    check(events.contains("destroy"), "wrapper-started service is destroyed");
    activity.startService(intent); drain();
    check(wrapper.stopService(intent), "wrapper stop delegates to activity"); drain();
    var receiver = new BroadcastReceiver() { public void onReceive(Context context, Intent intent) {} };
    for (int overload = 0; overload < 4; overload++) {
      var filter = new IntentFilter("test.WRAPPER_RECEIVER");
      switch (overload) {
        case 0 -> wrapper.registerReceiver(receiver, filter);
        case 1 -> wrapper.registerReceiver(receiver, filter, 0);
        case 2 -> wrapper.registerReceiver(receiver, filter, null, main);
        case 3 -> wrapper.registerReceiver(receiver, filter, null, main, 0);
      }
      activity.unregisterReceiver(receiver);
    }
    activity.registerReceiver(receiver, new IntentFilter("test.WRAPPER_RECEIVER"));
    wrapper.unregisterReceiver(receiver);
    check(true, "all wrapper receiver registration/unregister overloads use the base owner");
    events.clear();
  }
  private static <T> T callMain(Callable<T> work) throws Exception {
    var task = new FutureTask<>(work); main.post(task); return task.get(3, TimeUnit.SECONDS);
  }
  private static void componentOwners() throws Exception {
    var app = RuntimeEnvironment.get().application;
    var activity = new Activity(); activity.ventureAttach(app);
    var wrapper = new ContextWrapper(activity);
    declare(OwnerService.class, "test.game");
    var ownerIntent = new Intent(activity, OwnerService.class);
    activity.startService(ownerIntent); drain();
    Service service = ownerService; // Constructed/attached by LocalServices.Record.create.
    check(service != null && service.getBaseContext() != activity.getBaseContext()
        && service.getBaseContext() != app && activity.getBaseContext() != app,
        "production component attachment creates independent base contexts");
    check(activity.getBaseContext().ventureOuterContext() == activity
        && service.getBaseContext().ventureOuterContext() == service
        && wrapper.ventureOuterContext() == activity, "base contexts retain their outer components through wrappers");
    check(activity.getBaseContext().getResources() == app.getResources()
        && service.getBaseContext().getFilesDir().equals(app.getFilesDir())
        && service.getApplicationContext() == app, "component bases retain application environment access");
    var target = new Intent(activity, Worker.class).setAction("test.COMPONENT_OWNERS");
    var shared = new Client("shared");
    var activityCallbacks = new CopyOnWriteArrayList<Runnable>();
    var serviceCallbacks = new CopyOnWriteArrayList<Runnable>();
    check(wrapper.bindService(target, 1, activityCallbacks::add, shared)
        && service.bindService(target, 1, serviceCallbacks::add, shared), "attached components can share a ServiceConnection");
    drain();
    check(activityCallbacks.size() == 1 && serviceCallbacks.size() == 1,
        "each outer component retains its own executor and pending callback");
    activity.unbindService(shared); drain();
    check(!events.contains("unbind") && !events.contains("destroy"), "activity unbind leaves service's binding alive");
    callMain(() -> { activityCallbacks.get(0).run(); serviceCallbacks.get(0).run(); return null; });
    check(Collections.frequency(events, "connected:shared") == 1, "activity callback is cancelled while service callback remains deliverable");
    service.unbindService(shared); drain();
    check(events.contains("unbind") && events.contains("destroy"), "service owner releases the remaining binding");
    events.clear();

    var receipts = new CopyOnWriteArrayList<Context>();
    var receiver = new BroadcastReceiver() {
      public void onReceive(Context context, Intent intent) {
        if (!Looper.getMainLooper().isCurrentThread()) throw new AssertionError("Receiver off main");
        receipts.add(context);
      }
    };
    callMain(() -> {
      wrapper.registerReceiver(receiver, new IntentFilter("test.COMPONENT_RECEIVER"));
      service.registerReceiver(receiver, new IntentFilter("test.COMPONENT_RECEIVER"));
      activity.sendBroadcast(new Intent("test.COMPONENT_RECEIVER")); return null;
    }); drain();
    check(receipts.size() == 2 && receipts.contains(activity) && receipts.contains(service),
        "same receiver is independent per component and receives its outer context");
    receipts.clear();
    callMain(() -> {
      service.sendBroadcast(new Intent("test.COMPONENT_RECEIVER"));
      activity.unregisterReceiver(receiver); return null;
    }); drain();
    check(receipts.equals(List.of(service)), "activity unregister forgets only its pending receiver delivery");
    service.unregisterReceiver(receiver);

    var reentrant = new ServiceConnection() {
      public void onServiceConnected(ComponentName name, IBinder binder) {
        event("connected:reentrant"); activity.unbindService(this);
      }
      public void onServiceDisconnected(ComponentName name) { event("disconnected:reentrant"); }
    };
    check(wrapper.bindService(target, reentrant, 1), "callback-unbind fixture binds through attached activity wrapper");
    drain(); drain();
    check(Collections.frequency(events, "unbind") == 1 && Collections.frequency(events, "destroy") == 1
        && events.contains("connected:reentrant") && !events.contains("disconnected:reentrant"),
        "unbind inside onServiceConnected completes lifecycle without deadlock or death callback");
    activity.stopService(ownerIntent); drain(); events.clear();
  }
  private static SecondaryActivity secondaryActivity() throws Exception {
    return callMain(() -> {
      var activity = new SecondaryActivity();
      activity.ventureAttach(RuntimeEnvironment.get().application);
      activity.performLaunch();
      return activity;
    });
  }
  // AOSP finish() only requests destruction (VENTURE_MENU_QUIT default), so a
  // broadcast queued before it may legally reach the finishing activity.
  private static boolean beforeDestroy(Context context) {
    return AppMain.MENU_QUIT && context instanceof Activity a && a.isFinishing() && !a.isDestroyed();
  }
  private static void componentTeardown() throws Exception {
    declare(LeakedWorker.class, "test.game");
    var receipts = new CopyOnWriteArrayList<Context>();
    var receiver = new BroadcastReceiver() {
      public void onReceive(Context context, Intent intent) { if (!beforeDestroy(context)) receipts.add(context); }
    };
    var filter = new IntentFilter("test.TEARDOWN_RECEIVER");
    var broadcast = new Intent("test.TEARDOWN_RECEIVER");
    var activity = secondaryActivity();
    var target = new Intent(activity, LeakedWorker.class).setAction("test.LEAKED");
    var deferred = new CopyOnWriteArrayList<Runnable>();
    var client = new Client("leaked");
    events.clear(); leaks.clear();
    check(activity.bindService(target, 1, deferred::add, client), "secondary activity binds through production attach path");
    activity.registerReceiver(receiver, filter); drain();
    callMain(() -> { activity.sendBroadcast(broadcast); activity.finish(); return null; });
    drain(); drain();
    callMain(() -> { deferred.forEach(Runnable::run); return null; });
    check(activity.isDestroyed() && events.contains("activity-destroy"), "secondary activity runs onDestroy before context cleanup");
    check(events.contains("unbind") && events.contains("destroy"), "destroying last client automatically unbinds and destroys its service");
    activity.sendBroadcast(broadcast); drain();
    check(receipts.isEmpty() && !events.contains("connected:leaked"), "destroyed activity receives neither queued nor new callbacks");
    check(leaks.size() == 2 && leaks.stream().anyMatch(x -> x.startsWith("Leaked receiver "))
        && leaks.stream().anyMatch(x -> x.startsWith("Leaked ServiceConnection ")), "teardown logs each leaked registration once");

    var dying = secondaryActivity();
    var survivor = secondaryActivity();
    var dyingCallbacks = new CopyOnWriteArrayList<Runnable>();
    var survivingCallbacks = new CopyOnWriteArrayList<Runnable>();
    var shared = new Client("surviving");
    events.clear(); leaks.clear();
    check(dying.bindService(target, 1, dyingCallbacks::add, shared)
        && survivor.bindService(target, 1, survivingCallbacks::add, shared), "two attached activities share connection identity independently");
    dying.registerReceiver(receiver, filter); survivor.registerReceiver(receiver, filter); drain();
    callMain(() -> { dying.sendBroadcast(broadcast); dying.finish(); return null; }); drain(); drain();
    callMain(() -> { dyingCallbacks.forEach(Runnable::run); survivingCallbacks.forEach(Runnable::run); return null; });
    check(!events.contains("unbind") && !events.contains("destroy")
        && Collections.frequency(events, "connected:surviving") == 1, "destroyed owner is forgotten while another component retains service and callback");
    check(receipts.equals(List.of(survivor)) && leaks.size() == 2, "receiver cleanup affects only destroyed component");
    survivor.unregisterReceiver(receiver); survivor.unbindService(shared); drain();
    check(events.contains("unbind") && events.contains("destroy"), "surviving owner's final unbind destroys service");
    callMain(() -> { survivor.finish(); return null; }); drain();
    check(leaks.size() == 2, "explicitly released survivor has no leaks");

    var app = RuntimeEnvironment.get().application;
    var ownerIntent = new Intent(app, OwnerService.class);
    app.startService(ownerIntent); drain();
    Service service = ownerService;
    receipts.clear(); events.clear(); leaks.clear(); deferred.clear();
    check(service.bindService(target, 1, deferred::add, client), "service owns outgoing binding through production attachment");
    service.registerReceiver(receiver, filter); drain();
    check(app.stopService(ownerIntent), "stop owning service"); drain(); drain();
    callMain(() -> { deferred.forEach(Runnable::run); service.sendBroadcast(broadcast); return null; }); drain();
    check(events.contains("unbind") && events.contains("destroy") && !events.contains("connected:leaked")
        && receipts.isEmpty() && leaks.size() == 2, "service onDestroy also forgets leaked receiver and outgoing binding");
    events.clear();
  }
  private static void disconnectDestroysActivity() throws Exception {
    for (String replacement : List.of("test.CHANGED", "test.NULL")) {
      var activity = secondaryActivity();
      var callbacks = new CopyOnWriteArrayList<String>();
      var client = new ServiceConnection() {
        public void onServiceConnected(ComponentName name, IBinder binder) {
          callbacks.add(activity.isDestroyed() ? "late-connect" : "connect");
        }
        public void onNullBinding(ComponentName name) {
          callbacks.add(activity.isDestroyed() ? "late-null" : "null");
        }
        public void onServiceDisconnected(ComponentName name) {
          callbacks.add("disconnect"); activity.finish();
          callbacks.add(activity.isDestroyed() ? "destroyed" : "not-destroyed");
        }
      };
      events.clear(); leaks.clear();
      var intent = new Intent(activity, SwitchingWorker.class).setAction("test.INITIAL");
      check(activity.bindService(intent, client, 1), "attached activity receives initial Binder"); drain();
      check(activity.bindService(new Intent(intent).setAction(replacement), client, 1), "replacement Binder dispatch accepted");
      drain(); drain();
      // With AOSP's asynchronous finish() the replacement may still reach the
      // finishing activity, but never the destroyed one.
      check((AppMain.MENU_QUIT ? callbacks.subList(0, 3).equals(List.of("connect", "disconnect", "not-destroyed"))
              : callbacks.equals(List.of("connect", "disconnect", "destroyed")))
          && callbacks.stream().noneMatch(x -> x.startsWith("late-")) && activity.isDestroyed(),
          "disconnect-triggered destruction suppresses replacement callback, including null binding: " + callbacks);
      check(Collections.frequency(events, "activity-destroy") == 1
          && Collections.frequency(events, "destroy") == 1 && leaks.size() == 1,
          "disconnect teardown completes once and removes its leaked connection");
    }
    events.clear();
  }
  private static void explicitDestroyCleanup() throws Exception {
    var activity = callMain(() -> {
      var value = new ExplicitCleanupActivity();
      value.ventureAttach(RuntimeEnvironment.get().application); value.performLaunch(); return value;
    });
    activity.connection = new Client("explicit-destroy");
    var receipts = new CopyOnWriteArrayList<Context>();
    activity.receiver = new BroadcastReceiver() {
      public void onReceive(Context context, Intent intent) { if (!beforeDestroy(context)) receipts.add(context); }
    };
    events.clear(); leaks.clear();
    check(activity.bindService(new Intent(activity, Worker.class), activity.connection, 1), "explicit destroy cleanup fixture binds");
    activity.registerReceiver(activity.receiver, new IntentFilter("test.EXPLICIT_DESTROY")); drain();
    callMain(() -> {
      activity.sendBroadcast(new Intent("test.EXPLICIT_DESTROY")); activity.finish(); return null;
    }); drain(); drain();
    check(activity.isDestroyed() && Collections.frequency(events, "activity-destroy") == 1
        && Collections.frequency(events, "unbind") == 1 && Collections.frequency(events, "destroy") == 1,
        "explicit onDestroy unbind completes once without a second unbind");
    check(Collections.frequency(events, "connected:explicit-destroy") == 1 && receipts.isEmpty() && leaks.isEmpty(),
        "explicitly released registrations produce no leaks or late callbacks");
    events.clear();
  }
  private static void applicationOwnership() throws Exception {
    var app = RuntimeEnvironment.get().application;
    var activity = secondaryActivity();
    var target = new Intent(app, Worker.class).setAction("test.APPLICATION_OWNER");
    var client = new Client("application-owner");
    var appCallbacks = new CopyOnWriteArrayList<Runnable>();
    var activityCallbacks = new CopyOnWriteArrayList<Runnable>();
    var receipts = new CopyOnWriteArrayList<Context>();
    var receiver = new BroadcastReceiver() {
      public void onReceive(Context context, Intent intent) { if (!beforeDestroy(context)) receipts.add(context); }
    };
    events.clear(); leaks.clear();
    check(app.bindService(target, 1, appCallbacks::add, client)
        && activity.bindService(target, 1, activityCallbacks::add, client), "application and component independently own shared connection");
    app.registerReceiver(receiver, new IntentFilter("test.APPLICATION_RECEIVER"));
    activity.registerReceiver(receiver, new IntentFilter("test.APPLICATION_RECEIVER")); drain();
    callMain(() -> {
      activity.sendBroadcast(new Intent("test.APPLICATION_RECEIVER")); activity.finish(); return null;
    }); drain(); drain();
    callMain(() -> { appCallbacks.forEach(Runnable::run); activityCallbacks.forEach(Runnable::run); return null; });
    check(receipts.equals(List.of(app)) && Collections.frequency(events, "connected:application-owner") == 1
        && !events.contains("unbind") && !events.contains("destroy") && leaks.size() == 2,
        "application receiver and connection survive component cleanup with exact delivery counts");
    app.unregisterReceiver(receiver); app.unbindService(client); drain();
    check(Collections.frequency(events, "unbind") == 1 && Collections.frequency(events, "destroy") == 1,
        "application final unbind destroys service exactly once");
    events.clear();
  }
  private static void packageScopedService() throws Exception {
    var env = RuntimeEnvironment.get();
    var manifest = node("manifest", "package", "test.game");
    var app = node("application"); manifest.children.add(app);
    var service = node("service", "name", ".Filtered"); app.children.add(service);
    var filter = node("intent-filter"); service.children.add(filter);
    filter.children.add(node("action", "name", "test.LOCAL_SERVICE"));
    filter.children.add(node("category", "name", "test.SERVICE_CATEGORY"));
    var parsed = new PackageModel(manifest);
    var info = parsed.services.get(0); info.name = FilteredWorker.class.getName();
    info.applicationInfo = env.manifest.info.applicationInfo;
    env.manifest.services.add(info); env.manifest.serviceFilters.addAll(parsed.serviceFilters);
    var owner = new Context(); var client = new Client("package");
    var intent = new Intent("test.LOCAL_SERVICE").setPackage(owner.getPackageName()).addCategory("test.SERVICE_CATEGORY");
    check(owner.getPackageManager().resolveService(intent, 0).serviceInfo == info, "PackageManager resolves parsed manifest service filter");
    check(owner.bindService(intent, client, 1), "package-scoped filtered service binds"); drain();
    check(events.contains("connected:package") && events.contains("bind:test.LOCAL_SERVICE"), "package-scoped binding delivers connection");
    owner.unbindService(client); drain();
    check(!owner.bindService(new Intent(intent).setAction("test.NO_MATCH"), client, 1), "nonmatching package-scoped action cannot bind");
    check(!owner.bindService(new Intent(intent).setPackage("other.package"), client, 1), "foreign package cannot resolve local service filter");
    info.processName = "test.game:remote";
    check(!owner.bindService(intent, client, 1), "package-scoped resolution retains remote-process rejection");
    info.processName = "test.game";
    boolean implicit = false;
    try { owner.bindService(new Intent("test.LOCAL_SERVICE"), client, 1); }
    catch (IllegalArgumentException expected) { implicit = true; }
    check(implicit, "modern action-only service Intent still throws");
    int target = env.manifest.info.applicationInfo.targetSdkVersion;
    try {
      env.manifest.info.applicationInfo.targetSdkVersion = 20;
      check(owner.bindService(new Intent("test.LOCAL_SERVICE"), client, 1), "pre-Lollipop action-only service intent resolves locally"); drain();
      owner.unbindService(client); drain();
    } finally { env.manifest.info.applicationInfo.targetSdkVersion = target; }
    events.clear();
  }
  private static void binderUpdates() throws Exception {
    declare(SwitchingWorker.class, "test.game");
    var owner = new Context(); var client = new Client("binder");
    var intent = new Intent(owner, SwitchingWorker.class).setAction("test.SAME_ONE");
    check(owner.bindService(intent, client, 1), "first binder action binds"); drain();
    var first = client.binder;
    check(owner.bindService(new Intent(intent).setAction("test.SAME_TWO"), client, 1), "second action binds same component"); drain();
    check(Collections.frequency(events, "connected:binder") == 1 && !events.contains("disconnected:binder"), "same component and binder suppress redundant connection callback");
    check(owner.bindService(new Intent(intent).setAction("test.CHANGED"), client, 1), "changed binder action binds"); drain();
    check(client.binder != first && events.subList(events.size() - 2, events.size()).equals(List.of("disconnected:binder", "connected:binder")), "binder replacement disconnects then connects once");
    owner.unbindService(client); drain(); events.clear();
    var deferred = new CopyOnWriteArrayList<Runnable>();
    check(owner.bindService(intent, 0, deferred::add, client), "passive connection registers deferred executor");
    owner.startService(intent); drain();
    var initial = new FutureTask<Void>(() -> { deferred.remove(0).run(); return null; });
    main.post(initial); initial.get(3, TimeUnit.SECONDS);
    var oldBinder = client.binder;
    owner.stopService(intent); drain(); // Death notification waits on the executor.
    owner.startService(intent); drain();
    check(deferred.size() == 2, "old death and restarted connection are both queued");
    var reordered = new FutureTask<Void>(() -> {
      deferred.remove(1).run(); // Observe the new binder before the old death.
      deferred.remove(0).run();
      return null;
    });
    main.post(reordered); reordered.get(3, TimeUnit.SECONDS);
    check(client.binder != oldBinder && Collections.frequency(events, "disconnected:binder") == 1
        && events.get(events.size() - 1).equals("connected:binder"), "stale disconnect cannot remove the replacement binder");
    owner.unbindService(client); drain(); owner.stopService(intent); drain(); events.clear();
  }
  private static void exercise() throws Exception {
    var owner = new Context();
    var other = new Context();
    var intent = new Intent(owner, Worker.class).setAction("test.BIND");
    var first = new Client("first"); var second = new Client("second");
    check(!owner.bindService(new Intent().setClassName("missing.package", "Missing"), first, 1), "missing service rejects binding");
    var remote = declare(NullWorker.class, "test.game:worker");
    check(!owner.bindService(new Intent(owner, NullWorker.class), first, 1), "remote process cannot be silently co-located");
    remote.processName = "test.game"; remote.enabled = false;
    check(!owner.bindService(new Intent(owner, NullWorker.class), first, 1), "disabled service rejects binding");
    remote.enabled = true;
    check(!owner.stopService(intent), "stopping a service which does not exist returns false");
    check(owner.bindService(intent, first, 1) && owner.bindService(new Intent(intent).putExtra("ignored", 3), second, 1), "two clients bind");
    intent.setAction("test.MUTATED");
    drain();
    check(events.equals(List.of("create", "bind:test.BIND", "connected:first", "connected:second")), "create and bind once; callbacks follow on main and use intent snapshot");
    check(first.binder != null && first.binder == second.binder, "clients share cached Binder despite different extras");
    boolean foreign = false;
    try { other.unbindService(first); } catch (IllegalArgumentException expected) { foreign = true; }
    check(foreign, "connection belongs to registering Context");
    owner.unbindService(first); drain();
    check(!events.contains("unbind"), "one remaining client keeps service bound");
    owner.unbindService(second); drain();
    check(events.subList(events.size() - 2, events.size()).equals(List.of("unbind", "destroy")), "last auto client unbinds and destroys");
    check(events.stream().noneMatch(e -> e.startsWith("disconnected")), "ordinary unbind does not report service death");

    events.clear(); intent.setAction("test.BIND");
    check(owner.bindService(intent, first, 0), "passive bind registers while service is not started");
    drain(); check(events.isEmpty(), "passive bind cannot create a service");
    check(owner.startService(intent).equals(intent.getComponent()), "start returns explicit component");
    drain(); check(events.equals(List.of("create", "start:1", "bind:test.BIND", "connected:first")), "starting connects waiting passive client");
    owner.unbindService(first); drain();
    check(events.get(events.size() - 1).equals("unbind") && !events.contains("destroy"), "started service survives last unbind");
    check(owner.bindService(intent, second, 1), "started service rebind accepted"); drain();
    check(events.subList(events.size() - 2, events.size()).equals(List.of("rebind", "connected:second")), "onRebind reuses Binder rather than onBind");
    check(owner.stopService(intent), "stop accepts existing service"); drain();
    check(!events.contains("destroy"), "auto binding retains stopped service");
    owner.unbindService(second); drain(); check(events.get(events.size() - 1).equals("destroy"), "stopped service dies after last auto unbind");

    events.clear();
    var deferred = new CopyOnWriteArrayList<Runnable>();
    check(owner.bindService(intent, 1, deferred::add, first), "executor overload binds"); drain();
    check(deferred.size() == 1, "connection delivery is enqueued on supplied executor");
    owner.unbindService(first); drain(); deferred.get(0).run();
    check(events.stream().noneMatch(e -> e.startsWith("connected")), "unbind cancels already queued connection callback");
    var nullClient = new Client("null");
    check(owner.bindService(new Intent(owner, NullWorker.class), nullClient, 1), "null Binder service binds"); drain();
    check(events.contains("null:null") && !events.contains("connected:null"), "null Binder uses onNullBinding");
    owner.unbindService(nullClient); drain();

    events.clear();
    wrapperOwnership();
    componentOwners();
    packageScopedService();
    binderUpdates();
    componentTeardown();
    disconnectDestroysActivity();
    explicitDestroyCleanup();
    applicationOwnership();
    declare(ShutdownClientWorker.class, "test.game");
    var quitIntent = new Intent(owner, ShutdownClientWorker.class);
    check(owner.bindService(quitIntent, first, 1), "quit fixture binds"); drain();
    declare(ShutdownOwnerService.class, "test.game");
    declare(ShutdownWorker.class, "test.game");
    owner.startService(new Intent(owner, ShutdownOwnerService.class)); drain(); drain();
    check(Collections.frequency(events, "connected:shutdown-outgoing") == 1, "shutdown fixture service holds an outgoing binding");
    events.clear();
    var quitReceipts = new CopyOnWriteArrayList<Context>();
    var quitReceiver = new BroadcastReceiver() {
      public void onReceive(Context context, Intent value) { quitReceipts.add(context); }
    };
    owner.registerReceiver(quitReceiver, new IntentFilter("test.SHUTDOWN_RECEIVER"));
    var stopped = new CountDownLatch(1);
    main.post(() -> {
      owner.sendBroadcast(new Intent("test.SHUTDOWN_RECEIVER"));
      LocalServices.shutdown(); LocalServices.shutdown(); stopped.countDown();
    });
    check(stopped.await(3, TimeUnit.SECONDS), "quit teardown finishes");
    check(Collections.frequency(events, "shutdown-incoming-unbind") == 1
        && Collections.frequency(events, "shutdown-incoming-destroy") == 1,
        "app shutdown unbinds and destroys the app-bound service exactly once");
    check(Collections.frequency(events, "shutdown-outgoing-unbind") == 1
        && Collections.frequency(events, "shutdown-outgoing-destroy") == 1,
        "app shutdown unbinds and destroys the service-owned target exactly once");
    check(Collections.frequency(events, "shutdown-owner-destroy") == 1,
        "bounded app shutdown destroys the outgoing-binding owner exactly once");
    check(!owner.bindService(intent, second, 1), "shutdown rejects new service creation");
    owner.sendBroadcast(new Intent("test.SHUTDOWN_RECEIVER")); drain();
    check(quitReceipts.isEmpty(), "shutdown forgets queued and future receiver deliveries");
    boolean removed = false;
    try { owner.unregisterReceiver(quitReceiver); } catch (IllegalArgumentException expected) { removed = true; }
    check(removed, "shutdown removes receiver registrations");
  }
  public static void main(String[] args) throws Exception {
    var folder = Files.createTempDirectory("framework-service-test");
    Host host = (Host) java.lang.reflect.Proxy.newProxyInstance(Host.class.getClassLoader(), new Class<?>[]{Host.class},
        (proxy, method, values) -> {
          if (method.getName().equals("readAsset"))
            return "AndroidManifest.xml".equals(values[0]) ? FrameworkTest.obbManifest() : null;
          if (method.getName().equals("log")) {
            if (values[2].toString().startsWith("Leaked ")) leaks.add(values[2].toString());
            return null;
          }
          throw new UnsupportedOperationException(method.getName());
        });
    RuntimeEnvironment.configure(new String[]{"test.game", folder.resolve("base.apk").toString(),
        folder.resolve("data").toString(), folder.resolve("cache").toString(), folder.resolve("obb").toString(),
        folder.resolve("lib").toString(), "32", folder.resolve("app.jar").toString()}, host);
    // obbManifest intentionally omits uses-sdk; the host OS SDK argument does
    // not set an application's target SDK. Exercise the modern service rule.
    RuntimeEnvironment.get().manifest.info.applicationInfo.targetSdkVersion = 32;
    RuntimeEnvironment.get().loader = LocalServicesTest.class.getClassLoader();
    RuntimeEnvironment.get().application = new Application();
    RuntimeEnvironment.get().application.ventureAttach(new Context());
    declare(Worker.class, "test.game");
    Looper.prepareMainLooper(); main = new Handler(Looper.getMainLooper());
    var tester = new Thread(() -> {
      try { exercise(); } catch (Throwable error) { failure.set(error); }
      finally { main.post(() -> Looper.getMainLooper().quitSafely()); }
    }, "service-test-caller");
    tester.start(); Looper.loop(); tester.join(5000);
    if (failure.get() != null) throw new AssertionError("Service contracts failed", failure.get());
    System.out.println("LocalServicesTest: " + checks + " checks passed");
  }
}
