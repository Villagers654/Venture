import android.app.*;
import android.content.*;
import android.os.*;
import android.view.*;
import com.venture.runtime.*;
import java.util.*;

/** Runs the complete framework lifecycle independently of a guest native body. */
public final class LifecycleTest {
  static final List<String> events = new ArrayList<>();
  static final List<TestActivity> instances = new ArrayList<>();
  static final List<String> lifecycle = new ArrayList<>();
  static final List<String> results = new ArrayList<>();
  private static void pumpUntil(java.util.function.BooleanSupplier done) throws Exception {
    var next = MessageQueue.class.getDeclaredMethod("next");
    next.setAccessible(true);
    Handler handler = new Handler(Looper.getMainLooper());
    Runnable timeout = () -> { throw new AssertionError("Lifecycle callback timeout"); };
    handler.postDelayed(timeout, 3000);
    try {
      while (!done.getAsBoolean()) {
        Message message = (Message) next.invoke(Looper.myQueue());
        if (message == null) throw new AssertionError("Unexpected Looper shutdown");
        message.getTarget().dispatchMessage(message);
      }
    } finally { handler.removeCallbacks(timeout); }
  }
  private static void checkVisibleFrame(View view) {
    var frame = new android.graphics.Rect(-1, -1, -1, -1);
    view.getWindowVisibleDisplayFrame(frame);
    var metrics = view.getContext().getResources().getDisplayMetrics();
    if (frame.left != 0 || frame.top != 0 || frame.right != metrics.widthPixels || frame.bottom != metrics.heightPixels)
      throw new AssertionError("Visible window frame differs from the logical display");
  }
  public static class TestActivity extends Activity implements SurfaceHolder.Callback {
    public TestActivity() {
      instances.add(this);
    }
    protected void onCreate(Bundle b) {
      events.add("create");
      registerActivityLifecycleCallbacks(new Application.ActivityLifecycleCallbacks() {
        public void onActivityCreated(Activity a, Bundle b) {}
        public void onActivityStarted(Activity a) {}
        public void onActivityResumed(Activity a) {}
        public void onActivityPaused(Activity a) {}
        public void onActivityStopped(Activity a) {}
        public void onActivityDestroyed(Activity a) {}
        public void onActivitySaveInstanceState(Activity a, Bundle b) {}
        public void onActivityPostCreated(Activity a, Bundle b) { lifecycle.add("created"); }
        public void onActivityPostResumed(Activity a) { lifecycle.add("resumed"); }
        public void onActivityPrePaused(Activity a) { lifecycle.add("pausing"); }
        public void onActivityPostDestroyed(Activity a) { lifecycle.add("destroyed"); }
      });
      if (getBaseContext() == null)
        throw new AssertionError("base context");
      SurfaceView view = new SurfaceView(this);
      view.layout(3, 4, 10, 20);
      checkVisibleFrame(view); // Detached fallback uses display size, not child bounds.
      view.getHolder().addCallback(this);
      setContentView(view);
    }
    protected void onStart() {
      events.add("start");
    }
    protected void onPostCreate(Bundle b) { events.add("post-create"); }
    protected void onPostResume() { events.add("post-resume"); }
    protected void onActivityResult(int request, int result, Intent data) {
      results.add(request + ":" + result + ":" + data.getStringExtra("value"));
    }
    protected void onResume() {
      events.add("resume");
    }
    protected void onPause() {
      events.add("pause");
    }
    protected void onStop() {
      events.add("stop");
    }
    protected void onDestroy() {
      events.add("destroy");
    }
    public void onWindowFocusChanged(boolean focus) {
      events.add("focus:" + focus);
    }
    public void surfaceCreated(SurfaceHolder h) {
      events.add("surface-created");
      checkVisibleFrame(getWindow().getDecorView());
      if (!h.getSurface().isValid())
        throw new AssertionError("surface invalid");
    }
    public void surfaceChanged(SurfaceHolder h, int f, int w, int height) {
      events.add("surface-changed:" + w + "x" + height);
    }
    public void surfaceDestroyed(SurfaceHolder h) {
      events.add("surface-destroyed");
      if (!h.getSurface().isValid())
        throw new AssertionError("released before callback");
    }
  }
  public static void main(String[] args) throws Exception {
    try (RecordingHost host = new RecordingHost(args[1])) {
      RuntimeEnvironment.configure(args, host);
      Looper.prepareMainLooper();
      if (ActivityThread.currentActivityThread() != null)
        throw new AssertionError("ActivityThread exists before process attachment");
      ActivityThread.ventureAttach();
      ActivityThread processThread = ActivityThread.currentActivityThread();
      if (processThread == null || processThread.getApplication() != null)
        throw new AssertionError("process attachment before Application creation");
      Application app = new Application();
      app.ventureAttach(new Context());
      RuntimeEnvironment.get().application = app;
      var audioWorker = new Thread(() -> {
        if (ActivityThread.currentActivityThread() != processThread
            || processThread.getApplication() != app)
          throw new AssertionError("audio worker process Application identity");
      });
      var workerFailure = new java.util.concurrent.atomic.AtomicReference<Throwable>();
      audioWorker.setUncaughtExceptionHandler((t, failure) -> workerFailure.set(failure));
      audioWorker.start();
      audioWorker.join(3000);
      if (audioWorker.isAlive() || workerFailure.get() != null)
        throw new AssertionError("ActivityThread worker lookup", workerFailure.get());
      TestActivity activity = new TestActivity();
      activity.ventureAttach(app);
      activity.performLaunch();
      if (events.contains("surface-created") || events.contains("focus:true"))
        throw new AssertionError("Window callbacks ran inside Activity resume");
      pumpUntil(() -> events.contains("focus:true"));
      activity.performStop();
      activity.performResume();
      pumpUntil(() -> Collections.frequency(events, "focus:true") == 2);
      activity.finish();
      if (AppMain.MENU_QUIT && (!activity.isFinishing() || activity.isDestroyed() || events.contains("destroy")))
        throw new AssertionError("finish reentered application destruction");
      pumpUntil(activity::isDestroyed);
      List<String> expected = List.of("create", "start", "post-create", "resume", "post-resume", "surface-created",
          "surface-changed:1920x1080", "focus:true", "focus:false", "pause", "stop", "surface-destroyed",
          "start", "resume", "post-resume", "surface-created", "surface-changed:1920x1080", "focus:true",
          "focus:false", "pause", "stop", "surface-destroyed", "destroy");
      if (!lifecycle.equals(List.of("created", "resumed", "pausing", "resumed", "pausing", "destroyed")))
        throw new AssertionError("Activity-local lifecycle " + lifecycle);
      if (!events.equals(expected))
        throw new AssertionError(events.toString());
      if (host.events.stream().filter(e -> e.startsWith("surface ")).count() != 2
          || host.events.stream().filter(e -> e.startsWith("release ")).count() != 2)
        throw new AssertionError("surface ownership " + host.events);
      Context context = new Context();
      String hostFile = context.getFilesDir().toString();
      if (!context.getFilesDir().isAbsolute()
          || !RuntimeEnvironment.toGuestPath(hostFile).equals("/data/user/0/" + args[0] + "/files"))
        throw new AssertionError("file path mapping");
      System.out.println("PASS lifecycle " + String.join(" -> ", events));
      events.clear();
      TestActivity cancelled = new TestActivity();
      cancelled.ventureAttach(app);
      cancelled.performLaunch();
      cancelled.performPause();
      boolean[] frameRan = {false};
      Choreographer.getInstance().postFrameCallback(time -> frameRan[0] = true);
      pumpUntil(() -> frameRan[0]);
      if (events.contains("surface-created") || events.contains("focus:true") || events.contains("focus:false"))
        throw new AssertionError("Paused Activity received stale window callbacks " + events);
      cancelled.finish();
      pumpUntil(cancelled::isDestroyed);
      System.out.println("PASS paused Activity cancels pending window traversal/focus");
      var input = new android.view.inputmethod.BaseInputConnection(new View(context), true);
      input.setComposingText("abc", 1);
      input.setComposingText("ab", 1);
      input.commitText("hello", 1);
      input.setSelection(1, 4);
      input.commitText("x", 1);
      if (!input.getEditable().toString().equals("hxo"))
        throw new AssertionError("composition replacement");
      RuntimeEnvironment.configureDisplay(1920, 1080, 320, 72, true);
      HandlerThread thread = new HandlerThread("vsync-test");
      thread.start();
      var ready = new java.util.concurrent.CountDownLatch(1);
      var frame = new java.util.concurrent.CountDownLatch(1);
      new Handler(thread.getLooper()).post(() -> {
        Choreographer.getInstance().postFrameCallback(time -> {
          if (time != 123456789L)
            throw new AssertionError("host frame timestamp");
          frame.countDown();
        });
        ready.countDown();
      });
      if (!ready.await(3, java.util.concurrent.TimeUnit.SECONDS))
        throw new AssertionError("frame request");
      Choreographer.dispatchHostVsync(123456789L);
      if (!frame.await(3, java.util.concurrent.TimeUnit.SECONDS))
        throw new AssertionError("host vsync delivery");
      thread.quitSafely();
      thread.join(3000);
      System.out.println("PASS composition and host-vsync dispatch");
      var declaration = new android.content.pm.ActivityInfo();
      declaration.name = TestActivity.class.getName();
      declaration.packageName = args[0];
      RuntimeEnvironment.get().manifest.activities.add(declaration);
      events.clear();
      instances.clear();
      Intent launch = new Intent().setComponent(new ComponentName(args[0], declaration.name));
      AppMain.startActivity(launch);
      new Handler(Looper.getMainLooper()).post(() -> instances.get(0).startActivityForResult(launch, 71));
      new Handler(Looper.getMainLooper()).post(() -> {
        // The launch request above posts its transition, so check after that transition.
        new Handler(Looper.getMainLooper()).post(() -> {
        if (instances.size() != 2 || Collections.frequency(events, "pause") != 1
            || Collections.frequency(events, "stop") != 1)
          throw new AssertionError("in-process activity transition " + events);
        if (!instances.get(1).getCallingActivity().equals(instances.get(0).getComponentName()))
          throw new AssertionError("calling activity identity");
        instances.get(1).setResult(Activity.RESULT_OK, new Intent().putExtra("value", "completed"));
        instances.get(1).finish();
        if (AppMain.MENU_QUIT && (!results.isEmpty() || instances.get(1).isDestroyed()))
          throw new AssertionError("finish reentered result/destruction callbacks");
        new Handler(Looper.getMainLooper()).post(() -> {
        if (!results.equals(List.of("71:-1:completed"))) throw new AssertionError("activity result " + results);
        if (!instances.get(1).isDestroyed() || instances.get(0).isDestroyed()
            || Collections.frequency(events, "resume") != 3)
          throw new AssertionError("previous activity resume " + events);
        instances.get(0).finish();
        // Finishing the last activity must end the runtime's main loop itself.
        });
        });
      });
      Looper.loop();
      System.out.println("PASS in-process activity task and back navigation");
      checkPausedXrClock();
    }
  }
  private static void checkPausedXrClock() throws Exception {
    HandlerThread thread = new HandlerThread("paused-xr-clock");
    thread.start();
    try {
      Choreographer.dispatchHostVsync(System.nanoTime(), 1_000_000_000L / 72);
      var frame = new java.util.concurrent.CountDownLatch(1);
      var delivered = new java.util.concurrent.atomic.AtomicInteger();
      new Handler(thread.getLooper()).post(() ->
          Choreographer.getInstance().postFrameCallback(time -> {
            if (Looper.myLooper() != thread.getLooper())
              throw new AssertionError("frame callback left its owning Looper");
            delivered.incrementAndGet();
            frame.countDown();
          }));
      // A guest may wait for this Android pulse before its next xrWaitFrame.
      // The virtual display clock must keep running while XR pacing is paused.
      if (!frame.await(1, java.util.concurrent.TimeUnit.SECONDS))
        throw new AssertionError("Android frame stalled after the initial XR pulse");
      Choreographer.dispatchHostVsync(System.nanoTime(), 1_000_000_000L / 72);
      var drained = new java.util.concurrent.CountDownLatch(1);
      new Handler(thread.getLooper()).post(drained::countDown);
      if (!drained.await(1, java.util.concurrent.TimeUnit.SECONDS) || delivered.get() != 1)
        throw new AssertionError("host/fallback frame was delivered twice");
      System.out.println("PASS Android frames continue while XR pacing is paused");
    } finally {
      thread.quitSafely();
      thread.join(3000);
    }
  }
}
