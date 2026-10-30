import android.app.*;
import android.os.*;
import java.util.concurrent.atomic.*;

/** Unity permission-helper lookups/commits originate on the render thread. */
public final class FragmentPermissionsTest {
  private static void check(boolean value, String message) {
    if (!value) throw new AssertionError(message);
  }
  public static void main(String[] args) throws Exception {
    Looper.prepareMainLooper();
    Activity activity = new Activity() {
      public int checkSelfPermission(String permission) {
        return permission.equals("allowed") ? 0 : -1;
      }
    };
    FragmentManager manager = activity.getFragmentManager();
    AtomicInteger callbacks = new AtomicInteger();
    AtomicReference<Throwable> failure = new AtomicReference<>();
    Fragment permission = new Fragment() {
      public void onAttach(Activity owner) {
        check(Looper.myLooper() == Looper.getMainLooper(), "attachment runs on the UI Looper");
        requestPermissions(new String[]{"allowed", "denied"}, 42);
      }
      public void onRequestPermissionsResult(int request, String[] names, int[] results) {
        check(Looper.myLooper() == Looper.getMainLooper(), "permission result runs on UI Looper");
        check(request == 42 && java.util.Arrays.equals(names, new String[]{"allowed", "denied"}) &&
            java.util.Arrays.equals(results, new int[]{0, -1}), "actual permission results are preserved");
        callbacks.incrementAndGet();
        Thread lookup = new Thread(() -> {
          try {
            check(manager.findFragmentByTag("permission") == this, "render thread finds the attached helper");
            check(manager.getFragments().contains(this), "render thread reads an attached snapshot");
            manager.beginTransaction().remove(this).commit();
          } catch (Throwable error) { failure.set(error); }
        }, "UnityMain-query");
        lookup.start();
        try { lookup.join(2000); } catch (InterruptedException error) { throw new AssertionError(error); }
        check(!lookup.isAlive(), "lookup does not wait for a UI callback lock");
      }
      public void onDetach() {
        check(Looper.myLooper() == Looper.getMainLooper(), "removal runs on the UI Looper");
        callbacks.incrementAndGet(); Looper.getMainLooper().quitSafely();
      }
    };
    Thread render = new Thread(() -> {
      try {
        check(manager.findFragmentByTag("permission") == null, "initial render-thread lookup is allowed");
        // UnityPermissions uses the three-argument overload, with no view container.
        manager.beginTransaction().add(0, permission, "permission").commit();
        check(!permission.isAdded(), "render thread enqueues instead of executing attachment");
        boolean rejected = false;
        try { manager.executePendingTransactions(); } catch (IllegalStateException expected) { rejected = true; }
        check(rejected, "pending execution still requires the main Looper");
        rejected = false;
        try { manager.beginTransaction().add(new Fragment(), "wrong").commitNow(); }
        catch (IllegalStateException expected) { rejected = true; }
        check(rejected, "commitNow still requires the main Looper");
      } catch (Throwable error) { failure.set(error); }
    }, "UnityMain");
    render.start(); render.join(2000);
    check(!render.isAlive() && failure.get() == null, "Unity request did not throw: " + failure.get());
    new Handler(Looper.getMainLooper()).postDelayed(() -> { throw new AssertionError("permission result timeout"); }, 3000);
    Looper.loop();
    check(failure.get() == null, "background helper lookup failed: " + failure.get());
    check(callbacks.get() == 2 && manager.findFragmentByTag("permission") == null,
        "one result and one removal complete the helper lifecycle");
    check(manager.findFragmentByTag(null) == null, "null tags do not match anonymous fragments");
    boolean rejected = false;
    try { manager.beginTransaction().add(-1, new Fragment(), "invalid"); }
    catch (IllegalArgumentException expected) { rejected = true; }
    check(rejected, "View.NO_ID cannot identify a fragment container");
    Fragment headless = new Fragment();
    manager.beginTransaction().add(7, headless, "headless").commitNow();
    check(headless.isAdded() && manager.findFragmentByTag("headless") == headless,
        "headless fragments accept a named container as in upstream");
    manager.beginTransaction().remove(headless).commitNow();
    check(!headless.isAdded(), "named-container helper detaches normally");
    System.out.println("PASS Unity render-thread fragment permissions");
  }
}
