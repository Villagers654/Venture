package android.view.inputmethod;

import com.venture.runtime.SystemKeyboard;

// The input method is Venture's system keyboard (com.venture.runtime.SystemKeyboard):
// one per process, as on Android, whichever instance a caller holds.
public class InputMethodManager {
  public static final int SHOW_IMPLICIT = 1, SHOW_FORCED = 2, HIDE_IMPLICIT_ONLY = 1, HIDE_NOT_ALWAYS = 2;
  public static final int RESULT_UNCHANGED_SHOWN = 0, RESULT_UNCHANGED_HIDDEN = 1, RESULT_SHOWN = 2,
      RESULT_HIDDEN = 3;
  public boolean showSoftInput(android.view.View view, int flags) {
    return showSoftInput(view, flags, null);
  }
  public boolean showSoftInput(android.view.View view, int flags, android.os.ResultReceiver result) {
    boolean was = SystemKeyboard.isShown();
    boolean shown = SystemKeyboard.show(view);
    if (result != null)
      result.send(shown ? (was ? RESULT_UNCHANGED_SHOWN : RESULT_SHOWN) : RESULT_UNCHANGED_HIDDEN, null);
    return shown;
  }
  // Any window's token hides it: AOSP matches the served view's window, but a
  // game's field and the window it names need not share Venture's tokens, and a
  // keyboard that stays up over the game is worse than one that closes.
  public boolean hideSoftInputFromWindow(android.os.IBinder token, int flags) {
    return hideSoftInputFromWindow(token, flags, null);
  }
  public boolean hideSoftInputFromWindow(android.os.IBinder token, int flags, android.os.ResultReceiver result) {
    boolean hidden = SystemKeyboard.hide();
    if (result != null) result.send(hidden ? RESULT_HIDDEN : RESULT_UNCHANGED_HIDDEN, null);
    return hidden;
  }
  public boolean hideSoftInputFromWindow(Object token, int flags) {
    return SystemKeyboard.hide();
  }
  public void toggleSoftInput(int showFlags, int hideFlags) {
    SystemKeyboard.toggle();
  }
  public boolean isActive() {
    return SystemKeyboard.servedView() != null;
  }
  public boolean isActive(android.view.View v) {
    return v != null && SystemKeyboard.servedView() == v;
  }
  public boolean isAcceptingText() {
    return SystemKeyboard.isShown();
  }
  public void restartInput(android.view.View view) {
    SystemKeyboard.restart(view);
  }
  public InputMethodSubtype getCurrentInputMethodSubtype() { return new InputMethodSubtype(); }
  public void updateSelection(android.view.View view, int selStart, int selEnd, int candidatesStart, int candidatesEnd) {
    SystemKeyboard.textChanged(view);
  }
  public void updateExtractedText(android.view.View view, int token, ExtractedText text) {
    SystemKeyboard.textChanged(view);
  }
  public void dispatchKeyEventFromInputMethod(android.view.View view, android.view.KeyEvent event) {
    if (view != null) view.dispatchKeyEvent(event);
  }
}
