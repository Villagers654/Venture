package com.venture.runtime;

import android.app.Service;
import android.app.QueuedWork;
import android.content.*;
import android.content.pm.ServiceInfo;
import android.os.*;
import java.util.*;
import java.util.concurrent.Executor;

/** Process-local service lifecycle. Application callbacks run on the main Looper. */
public final class LocalServices {
  private LocalServices() {}
  // Only the caller-facing connection identities use this lock. Never call app
  // code under it: service callbacks can bind/unbind from other threads.
  private static final Object connectionsLock = new Object();
  private static final IdentityHashMap<Context, IdentityHashMap<ServiceConnection, Connection>> connections =
      new IdentityHashMap<>();
  private static volatile boolean shuttingDown;
  // Records may be reserved by callers; all mutable Record fields are confined
  // to main. Handler messages order lifecycle work.
  private static final Map<ComponentName, Record> records = new java.util.concurrent.ConcurrentHashMap<>();
  private record IntentKey(String action, String data, String type, String pkg, Set<String> categories) {
    IntentKey(Intent intent) {
      this(intent.getAction(), intent.getDataString(), intent.getType(), intent.getPackage(),
          intent.getCategories() == null ? Set.of() : Set.copyOf(intent.getCategories()));
    }
  }
  private static final class Connection {
    final ServiceConnection callback;
    final Executor executor;
    volatile boolean forgotten;
    private final Map<ComponentName, IBinder> active = new HashMap<>();
    Connection(ServiceConnection callback, Executor executor) {
      this.callback = callback;
      this.executor = executor;
    }
    void connected(Record record, IBinder binder) {
      executor.execute(() -> {
        IBinder previous;
        synchronized (this) {
          if (forgotten) return;
          previous = active.get(record.name);
          if (previous != null && previous == binder) return;
          if (binder == null) active.remove(record.name); else active.put(record.name, binder);
        }
        if (previous != null) {
          if (forgotten) return;
          callback.onServiceDisconnected(record.name);
        }
        // A disconnect callback may destroy its component or explicitly unbind.
        if (forgotten) return;
        android.util.Log.i("LocalServices", "connected " + record.name);
        if (forgotten) return;
        if (binder == null) callback.onNullBinding(record.name);
        else callback.onServiceConnected(record.name, binder);
      });
    }
    void disconnected(ComponentName name, IBinder binder) {
      executor.execute(() -> {
        synchronized (this) {
          if (forgotten || active.get(name) != binder) return;
          active.remove(name);
        }
        callback.onServiceDisconnected(name);
      });
    }
  }

  private static final class Binding {
    final Intent intent;
    final IdentityHashMap<Connection, Boolean> clients = new IdentityHashMap<>();
    IBinder binder;
    boolean requested, active, rebind;
    Binding(Intent intent) { this.intent = intent; }
  }
  private static final class Record {
    final ComponentName name;
    final Map<IntentKey, Binding> bindings = new LinkedHashMap<>();
    Service instance;
    boolean started;
    int startId;
    Record(ComponentName name) { this.name = name; }
    void create() {
      if (instance != null) return;
      try {
        var env = RuntimeEnvironment.get();
        instance = (Service) env.loader.loadClass(name.getClassName()).getDeclaredConstructor().newInstance();
        instance.ventureAttach(env.application == null ? new Context() : env.application);
        android.util.Log.i("LocalServices", "create " + name);
        instance.onCreate();
      } catch (ReflectiveOperationException failure) {
        throw new IllegalStateException("Unable to create service " + name, failure);
      }
    }
    void connect(Binding binding, Connection client) {
      if (instance == null || client.forgotten) return;
      if (!binding.requested) {
        binding.binder = instance.onBind(new Intent(binding.intent));
        binding.requested = true;
        android.util.Log.i("LocalServices", "bind " + name);
      } else if (!binding.active && binding.rebind) {
        instance.onRebind(new Intent(binding.intent));
        binding.rebind = false;
        android.util.Log.i("LocalServices", "rebind " + name);
      }
      binding.active = true;
      client.connected(this, binding.binder);
    }
    void connectWaiting() {
      for (var binding : bindings.values())
        for (var client : new ArrayList<>(binding.clients.keySet())) connect(binding, client);
    }
    void unbind(Binding binding) {
      if (!binding.active || instance == null) return;
      binding.active = false;
      binding.rebind = instance.onUnbind(new Intent(binding.intent));
      android.util.Log.i("LocalServices", "unbind " + name);
    }
    boolean needed() {
      if (started) return true;
      for (var binding : bindings.values())
        for (var client : binding.clients.entrySet())
          if (!client.getKey().forgotten && client.getValue()) return true;
      return false;
    }
    void destroy() {
      if (instance == null) return;
      Service old = instance;
      try {
        for (var binding : bindings.values()) unbind(binding);
      } finally {
        instance = null;
        for (var binding : bindings.values()) {
          if (binding.binder != null)
            for (var client : binding.clients.keySet()) client.disconnected(name, binding.binder);
          binding.requested = binding.active = binding.rebind = false;
          binding.binder = null;
        }
        android.util.Log.i("LocalServices", "destroy " + name);
        try { old.onDestroy(); }
        finally {
          try { removeContextRegistrations(old); }
          finally { QueuedWork.waitToFinish(); }
        }
      }
    }
  }
  private static Handler main() { return new Handler(Looper.getMainLooper()); }
  private static ComponentName resolve(Context owner, Intent intent) {
    Objects.requireNonNull(intent);
    if (intent.getComponent() == null) {
      // Android's modern explicit-service contract; external service discovery
      // is not represented by pretending that a local service exists.
      if (intent.getPackage() == null && owner.getApplicationInfo().targetSdkVersion >= 21)
        throw new IllegalArgumentException("Service Intent must be explicit");
    }
    var resolved = owner.getPackageManager().resolveService(intent, 0);
    ServiceInfo info = resolved == null ? null : resolved.serviceInfo;
    if (info == null || !owner.getPackageName().equals(info.packageName)
        || !info.enabled || !info.applicationInfo.enabled) return null;
    var name = new ComponentName(info.packageName, info.name);
    String process = info.processName == null ? info.applicationInfo.processName : info.processName;
    if (process != null && !process.equals(android.app.Application.getProcessName())) {
      android.util.Log.w("LocalServices", "Separate service process unavailable: " + name);
      return null;
    }
    return name;
  }
  public static boolean bind(Context owner, Intent intent, ServiceConnection callback, int flags, Executor executor) {
    Objects.requireNonNull(callback);
    Objects.requireNonNull(executor);
    ComponentName name = resolve(owner, intent);
    if (name == null || shuttingDown) return false;
    Context outer = owner.ventureOuterContext();
    Intent snapshot = new Intent(intent);
    synchronized (connectionsLock) {
      if (shuttingDown) return false;
      var owned = connections.computeIfAbsent(outer, k -> new IdentityHashMap<>());
      var client = owned.computeIfAbsent(callback, k -> new Connection(callback, executor));
      records.computeIfAbsent(name, Record::new);
      main().post(() -> {
        if (shuttingDown || client.forgotten) return;
        var record = records.computeIfAbsent(name, Record::new);
        var binding = record.bindings.computeIfAbsent(new IntentKey(snapshot), k -> new Binding(snapshot));
        boolean fresh = !binding.clients.containsKey(client);
        binding.clients.merge(client, (flags & Context.BIND_AUTO_CREATE) != 0, (a, b) -> a || b);
        if (record.needed() && record.instance == null) {
          record.create();
          record.connectWaiting();
        } else if (fresh) record.connect(binding, client);
      });
    }
    return true;
  }
  public static void unbind(Context owner, ServiceConnection callback) {
    Objects.requireNonNull(callback);
    Context outer = owner.ventureOuterContext();
    synchronized (connectionsLock) {
      var owned = connections.get(outer);
      var client = owned == null ? null : owned.remove(callback);
      if (client == null) throw new IllegalArgumentException("Service not registered");
      if (owned.isEmpty()) connections.remove(outer);
      client.forgotten = true; // Suppress connection callbacks already queued.
      main().post(() -> detachConnections(List.of(client)));
    }
  }
  // Main-Looper only. Snapshot iteration permits destruction callbacks to unbind.
  private static void detachConnections(Collection<Connection> clients) {
    for (var record : new ArrayList<>(records.values())) {
      for (var binding : record.bindings.values()) {
        boolean removed = false;
        for (var client : clients) removed |= binding.clients.remove(client) != null;
        if (removed && binding.clients.isEmpty()) record.unbind(binding);
      }
      if (!record.needed()) record.destroy();
      if (record.instance == null && record.bindings.values().stream().allMatch(b -> b.clients.isEmpty()))
        records.remove(record.name, record);
    }
  }
  /** Forget one component's leaked dispatchers before queued callbacks can run. */
  public static void removeContextRegistrations(Context owner) {
    Context outer = owner.ventureOuterContext();
    Context.ventureRemoveReceiverRegistrations(outer);
    List<Connection> leaked;
    synchronized (connectionsLock) {
      var owned = connections.remove(outer);
      if (owned == null) return;
      leaked = new ArrayList<>(owned.values());
      for (var client : leaked) client.forgotten = true;
    }
    for (var client : leaked)
      android.util.Log.w("LocalServices", "Leaked ServiceConnection " + client.callback.getClass().getName()
          + " owned by " + outer.getClass().getName() + "; unbinding");
    main().post(() -> detachConnections(leaked));
  }
  public static ComponentName start(Context owner, Intent intent) {
    ComponentName name = resolve(owner, intent);
    if (name == null || shuttingDown) return null;
    Intent snapshot = new Intent(intent);
    synchronized (connectionsLock) {
      if (shuttingDown) return null;
      records.computeIfAbsent(name, Record::new);
      main().post(() -> {
        if (shuttingDown) return;
        var record = records.computeIfAbsent(name, Record::new);
        record.started = true;
        boolean fresh = record.instance == null;
        record.create();
        record.instance.onStartCommand(snapshot, 0, ++record.startId);
        QueuedWork.waitToFinish();
        if (fresh) record.connectWaiting();
      });
    }
    return name;
  }
  public static boolean stop(Context owner, Intent intent) {
    ComponentName name = resolve(owner, intent);
    if (name == null || shuttingDown || !records.containsKey(name)) return false;
    main().post(() -> {
      var record = records.get(name);
      if (record == null) return;
      record.started = false;
      if (!record.needed()) {
        record.destroy();
        if (record.bindings.values().stream().allMatch(b -> b.clients.isEmpty())) records.remove(name, record);
      }
    });
    return true;
  }
  public static void stopSelf(Service service, int startId) {
    main().post(() -> {
      for (var record : records.values()) {
        if (record.instance != service || startId >= 0 && startId != record.startId) continue;
        record.started = false;
        if (!record.needed()) {
          record.destroy();
          if (record.bindings.values().stream().allMatch(b -> b.clients.isEmpty())) records.remove(record.name, record);
        }
        break;
      }
    });
  }
  /** Called by AppMain before quitting the main Looper, even on failed launch. */
  public static void shutdown() {
    if (!Looper.getMainLooper().isCurrentThread()) throw new IllegalStateException("Service teardown must run on main");
    synchronized (connectionsLock) {
      shuttingDown = true;
      for (var owned : connections.values()) for (var client : owned.values()) client.forgotten = true;
    }
    try {
      Throwable failure = null;
      for (var record : records.values()) {
        try { record.destroy(); }
        catch (RuntimeException | Error error) {
          if (failure == null) failure = error;
          else failure.addSuppressed(error);
        }
      }
      if (failure instanceof RuntimeException error) throw error;
      if (failure instanceof Error error) throw error;
    } finally {
      List<Context> owners;
      synchronized (connectionsLock) { owners = new ArrayList<>(connections.keySet()); }
      try { for (var owner : owners) removeContextRegistrations(owner); }
      finally {
        try { Context.ventureClearReceiverRegistrations(); }
        finally { records.clear(); }
      }
    }
  }
}
