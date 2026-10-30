package android.app;
import android.os.Handler;
import android.os.Looper;
import java.util.ArrayList;
import java.util.ArrayDeque;
import java.util.List;

public final class FragmentManager {
  private final Activity activity;
  private final List<Fragment> fragments = new ArrayList<>();
  private final ArrayDeque<Runnable> pending = new ArrayDeque<>();
  private int stage, sequence;
  private volatile boolean destroyed;
  FragmentManager(Activity activity) { this.activity = activity; }
  private void mainThread() {
    if (Looper.myLooper() != Looper.getMainLooper()) throw new IllegalStateException("Fragment operation off main thread");
  }
  public FragmentTransaction beginTransaction() { return new FragmentTransaction(this); }
  // AOSP allows lookups and deferred commits from any thread; Unity's
  // UnityPermissions.requestUserPermissions issues both from UnityMain.
  public Fragment findFragmentByTag(String tag) {
    if (tag == null) return null;
    synchronized (fragments) {
      for (int i = fragments.size() - 1; i >= 0; --i)
        if (java.util.Objects.equals(fragments.get(i).tag, tag)) return fragments.get(i);
    }
    return null;
  }
  public List<Fragment> getFragments() { synchronized (fragments) { return List.copyOf(fragments); } }
  public boolean isDestroyed() { return destroyed; }
  int commit(Runnable action, boolean now) {
    if (now) mainThread();
    int id;
    synchronized (pending) {
      if (destroyed) throw new IllegalStateException("Activity destroyed");
      id = sequence++;
      if (!now) pending.add(action);
    }
    if (now) action.run();
    else if (!new Handler(Looper.getMainLooper()).post(this::executePendingTransactions)) {
      synchronized (pending) { pending.removeLastOccurrence(action); }
      throw new IllegalStateException("Activity queue closed");
    }
    return id;
  }
  public boolean executePendingTransactions() {
    mainThread();
    boolean any = false;
    for (Runnable action; (action = nextPending()) != null; any = true) action.run();
    return any;
  }
  private Runnable nextPending() { synchronized (pending) { return pending.pollFirst(); } }
  void add(Fragment fragment, String tag) {
    mainThread();
    if (fragment.manager != null) throw new IllegalStateException("Fragment already added");
    fragment.manager = this; fragment.activity = activity; fragment.tag = tag;
    synchronized (fragments) { fragments.add(fragment); }
    fragment.onAttach((android.content.Context) activity);
    move(fragment, stage);
  }
  void remove(Fragment fragment) {
    mainThread();
    synchronized (fragments) {
      if (!fragments.remove(fragment)) throw new IllegalStateException("Fragment not added");
    }
    move(fragment, 0);
    fragment.onDetach(); fragment.manager = null; fragment.activity = null;
  }
  private void move(Fragment f, int target) {
    if (f.stage >= 3 && target < 3) { f.stage = 2; f.onPause(); }
    if (f.stage >= 2 && target < 2) { f.stage = 1; f.onStop(); }
    if (f.stage >= 1 && target < 1) { f.stage = 0; f.onDestroy(); }
    if (f.stage < 1 && target >= 1) {
      f.stage = 1; f.onCreate(null);
      if (f.onCreateView(new android.view.LayoutInflater(activity), null, null) != null)
        throw new UnsupportedOperationException("Fragment view containers unavailable");
      f.onActivityCreated(null);
    }
    if (f.stage < 2 && target >= 2) { f.stage = 2; f.onStart(); }
    if (f.stage < 3 && target >= 3) { f.stage = 3; f.onResume(); }
  }
  void dispatch(int target) {
    mainThread(); stage = target;
    for (Fragment f : getFragments()) if (f.manager == this) move(f, target);
  }
  void destroy() {
    mainThread();
    synchronized (pending) { destroyed = true; pending.clear(); }
    for (Fragment f : getFragments()) remove(f);
  }
}
