package android.window;
/**
 * API 31 splash screen control, as Activity.getSplashScreen() returns it.
 * A Quest app's VR activity opens with no starting window, so there is never
 * a splash screen view to hand over: an exit listener is kept but not called,
 * as AOSP does when no splash screen was shown.
 */
public interface SplashScreen {
  void setOnExitAnimationListener(OnExitAnimationListener listener);
  void clearOnExitAnimationListener();
  void setSplashScreenTheme(int themeId);

  interface OnExitAnimationListener {
    void onSplashScreenExit(SplashScreenView view);
  }

  /** The activity's splash screen state. */
  final class Impl implements SplashScreen {
    private OnExitAnimationListener listener;
    private int theme;
    @Override public synchronized void setOnExitAnimationListener(OnExitAnimationListener listener) {
      this.listener = listener;
    }
    @Override public synchronized void clearOnExitAnimationListener() { listener = null; }
    @Override public synchronized void setSplashScreenTheme(int themeId) { theme = themeId; }
    public synchronized OnExitAnimationListener listener() { return listener; }
    public synchronized int theme() { return theme; }
  }
}
