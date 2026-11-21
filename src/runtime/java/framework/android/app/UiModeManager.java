package android.app;
public class UiModeManager {
  public int getCurrentModeType() {
    return 1;
  }
  public int getNightMode() {
    return 1;
  }
  // Views ask whether colors are force-inverted for accessibility: never.
  public static final int FORCE_INVERT_TYPE_OFF = 0, FORCE_INVERT_TYPE_DARK = 1, FORCE_INVERT_TYPE_LIGHT = 2;
  public static final int FORCE_INVERT_PACKAGE_ALLOWED = 0, FORCE_INVERT_PACKAGE_ALWAYS_ENABLE = 1,
                          FORCE_INVERT_PACKAGE_ALWAYS_DISABLE = 2;
  public interface ForceInvertStateChangeListener {
    void onForceInvertStateChanged(int forceInvertState);
  }
  public int getForceInvertState() {
    return FORCE_INVERT_TYPE_OFF;
  }
  public int getForceInvertOverrideState() {
    return FORCE_INVERT_PACKAGE_ALLOWED;
  }
  public void addForceInvertStateChangeListener(java.util.concurrent.Executor executor,
                                                ForceInvertStateChangeListener listener) {}
  public void removeForceInvertStateChangeListener(ForceInvertStateChangeListener listener) {}
}
