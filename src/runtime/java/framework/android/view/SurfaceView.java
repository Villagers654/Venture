package android.view;
public class SurfaceView extends View {
  private final Holder holder = new Holder();
  public SurfaceView(android.content.Context c) {
    super(c);
  }
  public SurfaceView(android.content.Context c, android.util.AttributeSet a) {
    super(c, a);
  }
  public SurfaceHolder getHolder() {
    return holder;
  }
  protected void onAttachedToWindow() {
    holder.create();
  }
  protected void onDetachedFromWindow() {
    holder.destroy();
  }
  public void setZOrderOnTop(boolean top) {
    if (top)
      android.util.Log.w("SurfaceView", "Desktop overlay ordering unavailable");
  }
  public void setZOrderMediaOverlay(boolean overlay) {
    if (overlay)
      android.util.Log.w("SurfaceView", "Media overlay unavailable");
  }
  private final class Holder implements SurfaceHolder {
    private final java.util.List<Callback> callbacks =
        new java.util.concurrent.CopyOnWriteArrayList<>();
    private Surface surface;
    private int width = getResources().getDisplayMetrics().widthPixels,
                height = getResources().getDisplayMetrics().heightPixels, format = 1;
    private boolean creating;
    public void addCallback(Callback c) {
      if (!callbacks.contains(c))
        callbacks.add(c);
    }
    public void removeCallback(Callback c) {
      callbacks.remove(c);
    }
    public Surface getSurface() {
      return surface;
    }
    public android.graphics.Rect getSurfaceFrame() {
      return new android.graphics.Rect(0, 0, width, height);
    }
    public boolean isCreating() {
      return creating;
    }
    void create() {
      if (surface != null)
        return;
      surface = new Surface(width, height, format);
      creating = true;
      try {
        for (Callback c : callbacks) c.surfaceCreated(this);
        for (Callback c : callbacks) c.surfaceChanged(this, format, width, height);
      } finally {
        creating = false;
      }
    }
    void destroy() {
      if (surface == null)
        return;
      try {
        for (Callback c : callbacks) c.surfaceDestroyed(this);
      } finally {
        surface.release();
        surface = null;
      }
    }
    public void setFixedSize(int w, int h) {
      if (w <= 0 || h <= 0)
        throw new IllegalArgumentException();
      boolean live = surface != null;
      if (live)
        destroy();
      width = w;
      height = h;
      if (live)
        create();
    }
    public void setSizeFromLayout() {
      setFixedSize(getWidth(), getHeight());
    }
    public void setFormat(int f) {
      if (f != 1 && f != -1 && f != 4)
        throw new UnsupportedOperationException("Surface format " + f);
      int resolved = f == -1 ? 1 : f;
      if (resolved == format)
        return;
      boolean live = surface != null;
      if (live)
        destroy();
      format = resolved;
      if (live)
        create();
    }
    public void setType(int type) {
      if (type != 3)
        android.util.Log.w("SurfaceHolder", "Deprecated surface type " + type);
    }
    public void setKeepScreenOn(boolean k) {
      SurfaceView.this.setKeepScreenOn(k);
    }
  }
}
