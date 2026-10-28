package android.hardware.display;
public class DisplayManager {
  public interface DisplayListener {
    void onDisplayAdded(int id);
    void onDisplayRemoved(int id);
    void onDisplayChanged(int id);
  }
  private static final java.util.Map<DisplayListener, android.os.Handler> listeners =
      new java.util.concurrent.ConcurrentHashMap<>();
  public static void ventureDisplayChanged() {
    listeners.forEach((l, h) -> h.post(() -> l.onDisplayChanged(0)));
  }
  public android.view.Display getDisplay(int id) {
    return id == 0 ? new android.view.Display() : null;
  }
  public android.view.Display[] getDisplays() {
    return new android.view.Display[] {new android.view.Display()};
  }
  public void registerDisplayListener(DisplayListener l, android.os.Handler h) {
    listeners.put(l, h == null ? new android.os.Handler(android.os.Looper.getMainLooper()) : h);
  }
  public void unregisterDisplayListener(DisplayListener l) {
    listeners.remove(l);
  }
}
