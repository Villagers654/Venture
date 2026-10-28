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
}
