package android.window;
/** API 31 view of a shown splash screen, handed to SplashScreen.OnExitAnimationListener. */
public final class SplashScreenView extends android.widget.FrameLayout {
  private final android.view.View icon;
  private final java.time.Instant start = java.time.Instant.now();
  public SplashScreenView(android.content.Context context) {
    super(context);
    icon = null;
  }
  public java.time.Duration getIconAnimationDuration() { return java.time.Duration.ZERO; }
  public java.time.Instant getIconAnimationStart() { return start; }
  public android.view.View getIconView() { return icon; }
  public void remove() {
    final android.view.ViewParent parent = getParent();
    if (parent instanceof android.view.ViewGroup group) group.removeView(this);
  }
}
