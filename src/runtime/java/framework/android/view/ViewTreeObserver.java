package android.view;
public final class ViewTreeObserver {
  public interface OnGlobalLayoutListener {
    void onGlobalLayout();
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
  public boolean isAlive() {
    return true;
  }
  public void dispatchOnGlobalLayout() {
    for (var l : listeners) l.onGlobalLayout();
  }
}
