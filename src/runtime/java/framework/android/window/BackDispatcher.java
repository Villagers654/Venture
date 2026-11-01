package android.window;
/** Registered callbacks, highest priority last registered first, as WindowOnBackInvokedDispatcher. */
public final class BackDispatcher implements OnBackInvokedDispatcher {
  private record Entry(int priority, OnBackInvokedCallback callback) {}
  private final java.util.List<Entry> callbacks = new java.util.concurrent.CopyOnWriteArrayList<>();
  public void registerOnBackInvokedCallback(int priority, OnBackInvokedCallback callback) {
    if (priority < 0) throw new IllegalArgumentException("Application registered OnBackInvokedCallback cannot have negative priority");
    unregisterOnBackInvokedCallback(java.util.Objects.requireNonNull(callback));
    callbacks.add(new Entry(priority, callback));
  }
  public void unregisterOnBackInvokedCallback(OnBackInvokedCallback callback) {
    callbacks.removeIf(e -> e.callback == callback);
  }
  /** Invokes the top callback; false when none is registered. */
  public boolean dispatch() {
    Entry top = null;
    for (Entry e : callbacks) if (top == null || e.priority >= top.priority) top = e;
    if (top == null) return false;
    top.callback.onBackInvoked();
    return true;
  }
}
