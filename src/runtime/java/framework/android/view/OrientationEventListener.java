package android.view;
public abstract class OrientationEventListener {
  public static final int ORIENTATION_UNKNOWN = -1;
  public OrientationEventListener(android.content.Context c) {}
  public OrientationEventListener(android.content.Context c, int rate) {}
  public abstract void onOrientationChanged(int orientation);
  public boolean canDetectOrientation() {
    return false;
  }
  public void enable() {
    android.util.Log.w("OrientationEventListener", "No Android sensor adapter");
  }
  public void disable() {}
}
