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
  private boolean destroyed;
  FragmentManager(Activity activity) { this.activity = activity; }
  private void mainThread() {
    if (Looper.myLooper() != Looper.getMainLooper()) throw new IllegalStateException("Fragment operation off main thread");
  }
  public FragmentTransaction beginTransaction() { return new FragmentTransaction(this); }
  public Fragment findFragmentByTag(String tag) {
    mainThread();
    for (int i = fragments.size() - 1; i >= 0; --i)
      if (java.util.Objects.equals(fragments.get(i).tag, tag)) return fragments.get(i);
    return null;
  }
  public List<Fragment> getFragments() { mainThread(); return List.copyOf(fragments); }
  public boolean isDestroyed() { return destroyed; }
  int commit(Runnable action, boolean now) {
    mainThread();
    if (destroyed) throw new IllegalStateException("Activity destroyed");
    int id = sequence++;
    if (now) action.run();
    else {
      pending.add(action);
      if (!new Handler(Looper.getMainLooper()).post(this::executePendingTransactions)) {
        pending.removeLastOccurrence(action);
        throw new IllegalStateException("Activity queue closed");
      }
    }
    return id;
  }
  public boolean executePendingTransactions() {
    mainThread();
    boolean any = !pending.isEmpty();
    while (!pending.isEmpty()) pending.removeFirst().run();
    return any;
  }
  void add(Fragment fragment, String tag) {
    if (fragment.manager != null) throw new IllegalStateException("Fragment already added");
    fragment.manager = this; fragment.activity = activity; fragment.tag = tag;
    fragments.add(fragment);
    fragment.onAttach((android.content.Context) activity);
    move(fragment, stage);
  }
  void remove(Fragment fragment) {
    if (!fragments.remove(fragment)) throw new IllegalStateException("Fragment not added");
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
    for (Fragment f : List.copyOf(fragments)) if (f.manager == this) move(f, target);
  }
  void destroy() {
    mainThread(); destroyed = true; pending.clear();
    for (Fragment f : List.copyOf(fragments)) remove(f);
  }
}
