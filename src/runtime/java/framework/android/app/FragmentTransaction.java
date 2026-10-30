package android.app;
import java.util.ArrayList;
public final class FragmentTransaction {
  private final FragmentManager manager;
  private final ArrayList<Runnable> changes = new ArrayList<>();
  private boolean committed;
  FragmentTransaction(FragmentManager manager) { this.manager = manager; }
  public FragmentTransaction add(Fragment f, String tag) {
    return add(0, f, tag);
  }
  public FragmentTransaction add(int containerViewId, Fragment f) {
    return add(containerViewId, f, null);
  }
  public FragmentTransaction add(int containerViewId, Fragment f, String tag) {
    if (committed) throw new IllegalStateException("Already committed");
    // Unity uses zero for permission helpers. A named container is also valid
    // for a headless fragment; FragmentManager still refuses inflated views.
    if (containerViewId == android.view.View.NO_ID)
      throw new IllegalArgumentException("Fragment container has no ID");
    changes.add(() -> manager.add(f, tag)); return this;
  }
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
