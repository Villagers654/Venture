package android.view.inputmethod;
public class InputMethodManager {
  public boolean showSoftInput(android.view.View view, int flags) {
    return false;
  }
  public boolean hideSoftInputFromWindow(Object token, int flags) {
    return false;
  }
  public boolean hideSoftInputFromWindow(android.os.IBinder token, int flags) {
    return hideSoftInputFromWindow((Object) token, flags);
  }
  public boolean isActive() {
    return false;
  }
  public boolean isActive(android.view.View v) {
    return false;
  }
  public boolean isAcceptingText() {
    return false;
  }
  public void restartInput(android.view.View view) {
    android.util.Log.w("InputMethodManager", "Host IME unavailable");
  }
}
