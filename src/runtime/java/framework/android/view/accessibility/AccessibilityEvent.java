package android.view.accessibility;
public class AccessibilityEvent extends AccessibilityRecord {
  private int eventType;
  public static AccessibilityEvent obtain(int type) {
    AccessibilityEvent e = new AccessibilityEvent();
    e.eventType = type;
    return e;
  }
  public int getEventType() {
    return eventType;
  }
  public void recycle() {}
}
