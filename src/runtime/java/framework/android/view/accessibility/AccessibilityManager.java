package android.view.accessibility;
public class AccessibilityManager {
  public boolean isEnabled() {
    return false;
  }
  public boolean isTouchExplorationEnabled() {
    return false;
  }
  public interface AccessibilityStateChangeListener {
    void onAccessibilityStateChanged(boolean enabled);
  }
  public interface TouchExplorationStateChangeListener {
    void onTouchExplorationStateChanged(boolean enabled);
  }
  // Accessibility state never changes here, so listeners are kept but never called.
  private final java.util.Set<Object> listeners = java.util.concurrent.ConcurrentHashMap.newKeySet();
  public boolean addAccessibilityStateChangeListener(AccessibilityStateChangeListener l) { return listeners.add(l); }
  public boolean removeAccessibilityStateChangeListener(AccessibilityStateChangeListener l) { return listeners.remove(l); }
  public boolean addTouchExplorationStateChangeListener(TouchExplorationStateChangeListener l) { return listeners.add(l); }
  public boolean removeTouchExplorationStateChangeListener(TouchExplorationStateChangeListener l) { return listeners.remove(l); }
}
