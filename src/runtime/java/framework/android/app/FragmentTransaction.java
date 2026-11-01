package android.app;
import java.util.ArrayList;
public final class FragmentTransaction {
  private final FragmentManager manager;
  private final ArrayList<Runnable> changes = new ArrayList<>();
  private boolean committed;
  FragmentTransaction(FragmentManager manager) { this.manager = manager; }
  public FragmentTransaction add(Fragment f, String tag) {
    if (committed) throw new IllegalStateException("Already committed");
    changes.add(() -> manager.add(f, tag)); return this;
  }
  // Headless fragments have no view; a fragment that inflates one still fails
  // when it is attached, whichever container id was named.
  public FragmentTransaction add(int containerViewId, Fragment f, String tag) { return add(f, tag); }
  public FragmentTransaction add(int containerViewId, Fragment f) { return add(f, null); }
  public FragmentTransaction replace(int containerViewId, Fragment f) { return replace(containerViewId, f, null); }
  public FragmentTransaction replace(int containerViewId, Fragment f, String tag) {
    if (committed) throw new IllegalStateException("Already committed");
    changes.add(() -> { for (Fragment old : manager.getFragments()) manager.remove(old); manager.add(f, tag); });
    return this;
  }
  // No back stack, transitions or visibility without fragment views: recorded as no-ops.
  public FragmentTransaction addToBackStack(String name) { return this; }
  public FragmentTransaction disallowAddToBackStack() { return this; }
  public FragmentTransaction setTransition(int transit) { return this; }
  public FragmentTransaction setCustomAnimations(int enter, int exit) { return this; }
  public FragmentTransaction setCustomAnimations(int enter, int exit, int popEnter, int popExit) { return this; }
  public FragmentTransaction setReorderingAllowed(boolean allowed) { return this; }
  public FragmentTransaction hide(Fragment f) { return this; }
  public FragmentTransaction show(Fragment f) { return this; }
  public FragmentTransaction detach(Fragment f) { return this; }
  public FragmentTransaction attach(Fragment f) { return this; }
  public boolean isEmpty() { return changes.isEmpty(); }
  public FragmentTransaction remove(Fragment f) {
    if (committed) throw new IllegalStateException("Already committed");
    changes.add(() -> manager.remove(f)); return this;
  }
  private int commit(boolean now) {
    if (committed) throw new IllegalStateException("Already committed");
    committed = true;
    return manager.commit(() -> changes.forEach(Runnable::run), now);
  }
  public int commit() { return commit(false); }
  public int commitAllowingStateLoss() { return commit(false); }
  public void commitNow() { commit(true); }
  public void commitNowAllowingStateLoss() { commit(true); }
}
