package android.view;
public final class ViewTreeObserver {
  public interface OnGlobalLayoutListener {
    void onGlobalLayout();
  }
  public interface OnPreDrawListener {
    boolean onPreDraw();
  }
  public interface OnDrawListener {
    void onDraw();
  }
  private final java.util.List<OnGlobalLayoutListener> listeners =
      new java.util.concurrent.CopyOnWriteArrayList<>();
  public void addOnGlobalLayoutListener(OnGlobalLayoutListener l) {
    listeners.add(l);
  }
  public void removeOnGlobalLayoutListener(OnGlobalLayoutListener l) {
    listeners.remove(l);
  }
  public void removeGlobalOnLayoutListener(OnGlobalLayoutListener l) {
    listeners.remove(l);
  }
  private final java.util.List<OnPreDrawListener> preDrawListeners =
      new java.util.concurrent.CopyOnWriteArrayList<>();
  private final java.util.List<OnDrawListener> drawListeners =
      new java.util.concurrent.CopyOnWriteArrayList<>();
  public void addOnPreDrawListener(OnPreDrawListener l) { preDrawListeners.add(l); }
  public void removeOnPreDrawListener(OnPreDrawListener l) { preDrawListeners.remove(l); }
  public void addOnDrawListener(OnDrawListener l) { drawListeners.add(l); }
  public void removeOnDrawListener(OnDrawListener l) { drawListeners.remove(l); }
  // AOSP: every listener runs; any false return cancels this draw.
  public final boolean dispatchOnPreDraw() {
    boolean cancel = false;
    for (var l : preDrawListeners) cancel |= !l.onPreDraw();
    return cancel;
  }
  public final void dispatchOnDraw() {
    for (var l : drawListeners) l.onDraw();
  }
  // One traversal: layout, then the draw pass; there is no rendering, so a cancelled draw
  // is not retried.
  void dispatchTraversal() {
    dispatchOnGlobalLayout();
    if (!dispatchOnPreDraw()) dispatchOnDraw();
  }
  public boolean isAlive() {
    return true;
  }
  public void dispatchOnGlobalLayout() {
    for (var l : listeners) l.onGlobalLayout();
  }
}
