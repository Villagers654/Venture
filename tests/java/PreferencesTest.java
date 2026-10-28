import android.app.*;
import android.content.*;
import android.os.*;
import com.venture.runtime.*;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

/** Disk queue barriers exercise publication, ordering, failure and lifecycle drains. */
public final class PreferencesTest {
  private static int checks;
  private static Handler main;
  private static XmlPreferences preferences;
  private static final AtomicReference<Throwable> failure = new AtomicReference<>();
  private static void check(boolean ok, String message) {
    checks++;
    if (!ok) throw new AssertionError(message);
  }
  private static void await(CountDownLatch latch) {
    try { if (!latch.await(3, TimeUnit.SECONDS)) throw new AssertionError("Queue barrier timeout"); }
    catch (InterruptedException error) { throw new AssertionError(error); }
  }
  private static CountDownLatch blockDisk() {
    var entered = new CountDownLatch(1);
    var release = new CountDownLatch(1);
    QueuedWork.queue(() -> { entered.countDown(); await(release); }, false);
    await(entered);
    return release;
  }
  private static <T> FutureTask<T> onMain(Callable<T> work) {
    var task = new FutureTask<>(work);
    main.post(task);
    return task;
  }
  public static class SavingActivity extends Activity {
    protected void onPause() { preferences.edit().putString("lifecycle", "paused").apply(); }
    protected void onStop() { preferences.edit().putString("lifecycle", "stopped").apply(); }
  }
  private record Transition(int targetSdk, String state, boolean drains) {}
  private static volatile CountDownLatch serviceCallback;
  public static class SavingService extends Service {
    public IBinder onBind(Intent intent) { return new Binder(); }
    public int onStartCommand(Intent intent, int flags, int id) {
      preferences.edit().putString("service", "started").apply();
      serviceCallback.countDown();
      return START_NOT_STICKY;
    }
    public void onDestroy() {
      preferences.edit().putString("service", "stopped").apply();
      serviceCallback.countDown();
    }
  }
  private static void flag(Activity activity, String name) throws Exception {
    var field = Activity.class.getDeclaredField(name);
    field.setAccessible(true); field.setBoolean(activity, true);
  }
  private static void exercise(Path root, AtomicInteger writeFailures) throws Exception {
    Path file = root.resolve("preferences.xml");
    preferences = new XmlPreferences(file);
    var changes = new CopyOnWriteArrayList<String>();
    preferences.registerOnSharedPreferenceChangeListener((store, key) -> {
      if (!Looper.getMainLooper().isCurrentThread()) throw new AssertionError("Listener off main");
      changes.add(key);
    });
    var release = blockDisk();
    Thread committer = null;
    var committed = new AtomicBoolean();
    try {
      check(!QueuedWork.writesQuiet(0), "quiet query rejects pending work even with zero interval");
      onMain(() -> { preferences.edit().putInt("order", 1).apply(); return null; }).get(1, TimeUnit.SECONDS);
      check(preferences.getInt("order", 0) == 1 && changes.equals(List.of("order")), "apply publishes and notifies while disk worker is blocked");
      check(!Files.exists(file), "apply does not wait for a forced disk write");
      var set = new HashSet<>(Set.of("one"));
      preferences.edit().putInt("order", 2).putStringSet("set", set).apply(); set.add("late");
      committer = new Thread(() -> {
        committed.set(preferences.edit().putInt("order", 3).commit());
      }, "preference-committer");
      committer.start();
      // The editor is published before commit starts waiting for disk.
      long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(2);
      while (preferences.getInt("order", 0) != 3 && System.nanoTime() < deadline) Thread.yield();
      check(preferences.getInt("order", 0) == 3 && committer.isAlive() && !committed.get(), "commit publishes without holding the preferences monitor while waiting");
    } finally { release.countDown(); if (committer != null) committer.join(3000); }
    check(committed.get(), "commit completes after earlier applies");
    check(QueuedWork.finishPendingWrites() && QueuedWork.getFailureCount() == 0, "drain reports successful writes");
    check(QueuedWork.writesQuiet(0) && !QueuedWork.writesQuiet(TimeUnit.DAYS.toMillis(1)), "quiet query uses last write completion and requested interval");
    Thread.sleep(10);
    check(QueuedWork.writesQuiet(1), "quiet interval expires while idle");
    var reloaded = new XmlPreferences(file);
    check(reloaded.getInt("order", 0) == 3 && reloaded.getStringSet("set", Set.of()).equals(Set.of("one")), "ordered final snapshot and defensive sets persist");
    var reusable = preferences.edit().clear().putInt("old", 1);
    reusable.apply(); preferences.edit().remove("old").putInt("other", 2).apply(); reusable.putInt("new", 3).apply();
    QueuedWork.waitToFinish();
    reloaded = new XmlPreferences(file);
    check(!reloaded.contains("old") && reloaded.getInt("other", 0) == 2 && reloaded.getInt("new", 0) == 3, "reused Editor consumes its clear flag and updates once");

    // A queued SDK task may make a synchronous final save. An older apply can
    // already be waiting behind that task when the nested commit is published.
    var workerEntered = new CountDownLatch(1);
    var allowCommit = new CountDownLatch(1);
    var workerCommit = new FutureTask<Boolean>(() -> {
      workerEntered.countDown(); await(allowCommit);
      return preferences.edit().putInt("worker", 1).commit();
    });
    QueuedWork.queue(workerCommit, false); await(workerEntered);
    preferences.edit().putInt("worker", 0).apply();
    allowCommit.countDown();
    check(workerCommit.get(3, TimeUnit.SECONDS), "commit on the QueuedWork worker completes without waiting for itself");
    check(new XmlPreferences(file).getInt("worker", -1) == 1, "nested synchronous commit is durable before returning");
    var afterCommit = new FutureTask<Boolean>(() -> { QueuedWork.waitToFinish(); return true; });
    var drainer = new Thread(afterCommit, "nested-commit-drainer"); drainer.setDaemon(true); drainer.start();
    check(afterCommit.get(3, TimeUnit.SECONDS) && !QueuedWork.hasPendingWork(), "waitToFinish after nested commit completes with balanced counters");
    check(new XmlPreferences(file).getInt("worker", -1) == 1, "older queued snapshots cannot overwrite the newer inline commit");

    Path workerApplyFile = root.resolve("worker-apply.xml");
    var workerApplyPrefs = new XmlPreferences(workerApplyFile);
    var applyingTaskEntered = new CountDownLatch(1);
    var allowApply = new CountDownLatch(1);
    var appliedOnWorker = new CountDownLatch(1);
    var releaseApplyingTask = new CountDownLatch(1);
    QueuedWork.queue(() -> {
      applyingTaskEntered.countDown(); await(allowApply);
      workerApplyPrefs.edit().putInt("async", 1).apply();
      appliedOnWorker.countDown(); await(releaseApplyingTask);
    }, false);
    try {
      await(applyingTaskEntered);
      // This older snapshot is already queued behind the active SDK task.
      workerApplyPrefs.edit().putInt("async", 0).apply();
      allowApply.countDown();
      await(appliedOnWorker);
      check(workerApplyPrefs.getInt("async", 0) == 1 && !Files.exists(workerApplyFile),
          "apply on the worker publishes memory and leaves its disk write queued");
    } finally { allowApply.countDown(); releaseApplyingTask.countDown(); }
    QueuedWork.waitToFinish();
    check(new XmlPreferences(workerApplyFile).getInt("async", 0) == 1, "worker-side apply runs after the older waiting write and leaves the later value on disk");

    var finishing = new CountDownLatch(1);
    var finishRelease = new CountDownLatch(1);
    QueuedWork.addFinisher(() -> { finishing.countDown(); await(finishRelease); });
    var firstDrain = new FutureTask<Boolean>(QueuedWork::finishPendingWrites);
    var interruptRestored = new AtomicBoolean();
    var secondDrain = new FutureTask<Boolean>(() -> {
      Thread.currentThread().interrupt();
      boolean success = QueuedWork.finishPendingWrites();
      interruptRestored.set(Thread.currentThread().isInterrupted());
      return success;
    });
    new Thread(firstDrain, "finisher-first-waiter").start(); await(finishing);
    try {
      new Thread(secondDrain, "finisher-second-waiter").start();
      boolean waited = false;
      try { secondDrain.get(50, TimeUnit.MILLISECONDS); } catch (TimeoutException expected) { waited = true; }
      check(waited && !QueuedWork.writesQuiet(0), "concurrent drain and quiet query include an executing finisher");
    } finally { finishRelease.countDown(); }
    check(firstDrain.get(3, TimeUnit.SECONDS) && secondDrain.get(3, TimeUnit.SECONDS) && interruptRestored.get(),
        "both drain callers finish accepted work and restore interruption");

    // Force deterministic IO failure without mocking the serializer or adding a
    // product test hook: a regular file cannot be the preference directory.
    Path obstacle = root.resolve("not-a-directory"); Files.writeString(obstacle, "test");
    var broken = new XmlPreferences(obstacle.resolve("failed.xml"));
    broken.edit().putBoolean("memory", true).apply();
    check(!QueuedWork.finishPendingWrites() && QueuedWork.getFailureCount() == 1, "drain reports asynchronous IO failure");
    check(broken.getBoolean("memory", false) && writeFailures.get() == 1, "failed asynchronous write logs and keeps published memory without throwing from apply");
    check(!broken.edit().putBoolean("memory", false).commit() && !broken.getBoolean("memory", true), "commit returns disk failure while retaining memory publication");

    long beforeInlineFailure = QueuedWork.getFailureCount();
    var inlineFailure = new FutureTask<Boolean>(() -> broken.edit().putInt("inline-failure", 1).commit());
    QueuedWork.queue(inlineFailure, false);
    check(!inlineFailure.get(3, TimeUnit.SECONDS), "worker-side failed synchronous commit returns false without stalling");
    QueuedWork.waitToFinish();
    check(QueuedWork.getFailureCount() == beforeInlineFailure + 1 && !QueuedWork.hasPendingWork()
        && broken.getInt("inline-failure", 0) == 1,
        "inline failure is counted exactly once, balances pending work and retains memory publication");

    SavingActivity activity = onMain(() -> { var a = new SavingActivity(); a.ventureAttach(RuntimeEnvironment.get().application); return a; }).get();
    for (var transition : List.of(new Transition(10, "paused", true), new Transition(32, "paused", false),
                                 new Transition(32, "stopped", true), new Transition(10, "stopped", false))) {
      String state = transition.state();
      release = blockDisk();
      var entered = new CountDownLatch(1);
      try {
        var task = onMain(() -> {
          RuntimeEnvironment.get().manifest.info.applicationInfo.targetSdkVersion = transition.targetSdk();
          flag(activity, state.equals("paused") ? "resumed" : "started");
          entered.countDown();
          if (state.equals("paused")) activity.performPause(); else activity.performStop();
          return null;
        });
        await(entered);
        if (transition.drains()) {
          boolean waited = false;
          try { task.get(50, TimeUnit.MILLISECONDS); } catch (TimeoutException expected) { waited = true; }
          check(waited, "target " + transition.targetSdk() + " " + state + " drains pending writes");
        } else {
          task.get(1, TimeUnit.SECONDS);
          check(QueuedWork.hasPendingWork(), "target " + transition.targetSdk() + " " + state + " leaves work for its AOSP drain boundary");
        }
        release.countDown(); task.get(3, TimeUnit.SECONDS);
        QueuedWork.waitToFinish();
        check(new XmlPreferences(file).getString("lifecycle", "").equals(state), "lifecycle " + state + " persists before return");
      } finally { release.countDown(); }
    }
    var info = new android.content.pm.ServiceInfo();
    info.name = SavingService.class.getName(); info.packageName = "test.game";
    info.applicationInfo = RuntimeEnvironment.get().manifest.info.applicationInfo;
    RuntimeEnvironment.get().manifest.services.add(info);
    var context = new Context(); var serviceIntent = new Intent(context, SavingService.class);
    for (String state : List.of("started", "stopped")) {
      release = blockDisk(); serviceCallback = new CountDownLatch(1);
      try {
        if (state.equals("started")) context.startService(serviceIntent); else context.stopService(serviceIntent);
        await(serviceCallback);
        var barrier = onMain(() -> null);
        boolean waited = false;
        try { barrier.get(50, TimeUnit.MILLISECONDS); } catch (TimeoutException expected) { waited = true; }
        check(waited, "service " + state + " handler drains pending writes");
        release.countDown(); barrier.get(3, TimeUnit.SECONDS);
        check(new XmlPreferences(file).getString("service", "").equals(state), "service " + state + " persists before handling completes");
      } finally { release.countDown(); }
    }
    var nested = new AtomicBoolean();
    QueuedWork.queue(() -> QueuedWork.queue(() -> nested.set(true), false), false);
    QueuedWork.waitToFinish(); check(nested.get() && !QueuedWork.hasPendingWork(), "drain includes work enqueued by queued work");
    preferences.edit().putString("shutdown", "saved").apply();
    onMain(() -> { AppMain.shutdown(); return null; }).get(3, TimeUnit.SECONDS);
    check(new XmlPreferences(file).getString("shutdown", "").equals("saved"), "AppMain shutdown drains without an Activity");
    check(!QueuedWork.finishPendingWrites() && QueuedWork.getFailureCount() == 3, "later successes do not hide earlier failed writes");
  }
  public static void main(String[] args) throws Exception {
    Path root = Files.createTempDirectory("framework-preferences-test");
    var writeFailures = new AtomicInteger();
    Host host = (Host) java.lang.reflect.Proxy.newProxyInstance(Host.class.getClassLoader(), new Class<?>[]{Host.class},
        (proxy, method, values) -> {
          if (method.getName().equals("readAsset")) return "AndroidManifest.xml".equals(values[0]) ? FrameworkTest.obbManifest() : null;
          if (method.getName().equals("log")) { if (values[2].toString().startsWith("Commit failed:")) writeFailures.incrementAndGet(); return null; }
          throw new UnsupportedOperationException(method.getName());
        });
    RuntimeEnvironment.configure(new String[]{"test.game", root.resolve("base.apk").toString(), root.resolve("data").toString(),
        root.resolve("cache").toString(), root.resolve("obb").toString(), root.resolve("lib").toString(), "32", root.resolve("app.jar").toString()}, host);
    RuntimeEnvironment.get().application = new Application(); RuntimeEnvironment.get().application.ventureAttach(new Context());
    RuntimeEnvironment.get().loader = PreferencesTest.class.getClassLoader();
    Looper.prepareMainLooper(); main = new Handler(Looper.getMainLooper());
    Thread tester = new Thread(() -> {
      try { exercise(root, writeFailures); } catch (Throwable error) { failure.set(error); }
      finally { main.post(() -> Looper.getMainLooper().quitSafely()); }
    }, "preferences-test-caller");
    tester.start(); Looper.loop(); tester.join(5000);
    if (failure.get() != null) throw new AssertionError("Preference contracts failed", failure.get());
    System.out.println("PreferencesTest: " + checks + " checks passed");
  }
}
