package android.hardware.display;

import android.os.Handler;
import android.os.Looper;
import android.view.Display;
import android.view.Surface;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

/**
 * AOSP android.hardware.display.DisplayManager: the default display, and the
 * virtual displays an app creates on a Surface it owns (a video encoder, or an
 * XR quad's Surface for a Presentation). A virtual display's windows are drawn
 * into its Surface by the process's own window compositor
 * (com.venture.runtime.Components.virtualDisplay).
 */
public class DisplayManager {
  public static final String DISPLAY_CATEGORY_PRESENTATION = "android.hardware.display.category.PRESENTATION";
  public static final String DISPLAY_CATEGORY_ALL_INCLUDING_DISABLED = "android.hardware.display.category.ALL_INCLUDING_DISABLED";
  public static final int VIRTUAL_DISPLAY_FLAG_PUBLIC = 1, VIRTUAL_DISPLAY_FLAG_PRESENTATION = 1 << 1,
      VIRTUAL_DISPLAY_FLAG_SECURE = 1 << 2, VIRTUAL_DISPLAY_FLAG_OWN_CONTENT_ONLY = 1 << 3,
      VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR = 1 << 4;

  public interface DisplayListener {
    void onDisplayAdded(int id);
    void onDisplayRemoved(int id);
    void onDisplayChanged(int id);
  }
  private static final Map<DisplayListener, Handler> listeners = new ConcurrentHashMap<>();
  // Display IDs above Android's default (0), in creation order, as DisplayManagerService numbers them.
  private static final Map<Integer, VirtualDisplay> virtualDisplays = new ConcurrentHashMap<>();
  private static int nextDisplayId = 2;

  public static void ventureDisplayChanged() {
    listeners.forEach((l, h) -> h.post(() -> l.onDisplayChanged(0)));
  }
  private static void notify(int id, int change) {
    listeners.forEach((l, h) -> h.post(() -> {
      if (change == 0) l.onDisplayAdded(id);
      else if (change == 1) l.onDisplayRemoved(id);
      else l.onDisplayChanged(id);
    }));
  }

  public Display getDisplay(int id) {
    if (id == 0) return com.venture.runtime.Components.defaultDisplay();
    VirtualDisplay display = virtualDisplays.get(id);
    return display == null ? null : display.getDisplay();
  }
  public Display[] getDisplays() { return getDisplays(null); }
  /** All displays; for the presentation category, the virtual displays made for presentations. */
  public Display[] getDisplays(String category) {
    List<Display> displays = new ArrayList<>();
    boolean presentation = DISPLAY_CATEGORY_PRESENTATION.equals(category);
    if (!presentation) displays.add(com.venture.runtime.Components.defaultDisplay());
    for (VirtualDisplay display : virtualDisplays.values())
      if (!presentation || (display.flags & VIRTUAL_DISPLAY_FLAG_PRESENTATION) != 0) displays.add(display.getDisplay());
    return displays.toArray(new Display[0]);
  }
  public void registerDisplayListener(DisplayListener l, Handler h) {
    listeners.put(l, h == null ? new Handler(Looper.getMainLooper()) : h);
  }
  public void unregisterDisplayListener(DisplayListener l) {
    listeners.remove(l);
  }

  public VirtualDisplay createVirtualDisplay(String name, int width, int height, int densityDpi, Surface surface,
      int flags) {
    return createVirtualDisplay(name, width, height, densityDpi, surface, flags, null, null);
  }
  public VirtualDisplay createVirtualDisplay(String name, int width, int height, int densityDpi, Surface surface,
      int flags, VirtualDisplay.Callback callback, Handler handler) {
    if (name == null || name.isEmpty()) throw new IllegalArgumentException("name must be non-null and non-empty");
    if (width <= 0 || height <= 0 || densityDpi <= 0)
      throw new IllegalArgumentException("width, height, and densityDpi must be greater than 0");
    if (surface != null && !surface.isValid()) throw new IllegalArgumentException("Surface is not valid");
    int id;
    synchronized (DisplayManager.class) { id = nextDisplayId++; }
    Display display = com.venture.runtime.Components.virtualDisplay(id, name, width, height, densityDpi, flags, surface);
    VirtualDisplay virtual = new VirtualDisplay(id, display, width, height, densityDpi, surface, flags, callback, handler);
    virtualDisplays.put(id, virtual);
    notify(id, 0);
    return virtual;
  }

  static void virtualDisplayChanged(VirtualDisplay display) {
    com.venture.runtime.Components.virtualDisplayChanged(display.id, display.getSurface(), display.width,
        display.height, display.densityDpi);
    notify(display.id, 2);
  }
  static void virtualDisplayReleased(VirtualDisplay display) {
    if (virtualDisplays.remove(display.id) == null) return;
    com.venture.runtime.Components.virtualDisplayReleased(display.id);
    notify(display.id, 1);
  }
}
