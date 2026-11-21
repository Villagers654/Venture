package android.hardware.display;

import android.os.Handler;
import android.os.Looper;
import android.view.Display;
import android.view.Surface;

/** AOSP android.hardware.display.VirtualDisplay: a display drawn into a Surface its creator owns. */
public final class VirtualDisplay {
  public static abstract class Callback {
    public void onPaused() {}
    public void onResumed() {}
    public void onStopped() {}
  }

  final int id, flags;
  int width, height, densityDpi;
  private final Display display;
  private final Callback callback;
  private final Handler handler;
  private Surface surface;
  private boolean released;

  VirtualDisplay(int id, Display display, int width, int height, int densityDpi, Surface surface, int flags,
      Callback callback, Handler handler) {
    this.id = id;
    this.display = display;
    this.surface = surface;
    this.flags = flags;
    this.callback = callback;
    this.handler = handler != null ? handler : new Handler(Looper.getMainLooper());
    this.width = width;
    this.height = height;
    this.densityDpi = densityDpi;
    if (callback != null) this.handler.post(surface != null ? callback::onResumed : callback::onPaused);
  }

  public Display getDisplay() { return display; }
  public synchronized Surface getSurface() { return surface; }

  /** Moves the display to another Surface; null pauses it, as AOSP does. */
  public void setSurface(Surface surface) {
    synchronized (this) {
      if (released || this.surface == surface) return;
      this.surface = surface;
    }
    DisplayManager.virtualDisplayChanged(this);
    if (callback != null) handler.post(surface != null ? callback::onResumed : callback::onPaused);
  }

  public void resize(int width, int height, int densityDpi) {
    if (width <= 0 || height <= 0 || densityDpi <= 0)
      throw new IllegalArgumentException("width, height, and densityDpi must be greater than 0");
    synchronized (this) {
      if (released) return;
      this.width = width;
      this.height = height;
      this.densityDpi = densityDpi;
    }
    DisplayManager.virtualDisplayChanged(this);
  }

  public void release() {
    synchronized (this) {
      if (released) return;
      released = true;
    }
    DisplayManager.virtualDisplayReleased(this);
    if (callback != null) handler.post(callback::onStopped);
  }

  @Override public String toString() {
    return "VirtualDisplay{display=" + display + ", surface=" + surface + "}";
  }
}
