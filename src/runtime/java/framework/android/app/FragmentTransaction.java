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
