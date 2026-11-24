package android.view.inputmethod;

import android.os.IBinder;
import android.os.ResultReceiver;
import android.view.View;
import com.venture.runtime.SystemKeyboard;
import com.venture.runtime.layoutlib.PanelHost;

/**
 * The input method of a 2D app: Venture's system keyboard
 * (com.venture.runtime.SystemKeyboard), as for a VR game. InputMethodManager's
 * own code runs (its focus tracking, its connections for the desktop window's
 * typing); only showing and hiding the soft keyboard come here
 * (layoutlib-rules.txt keeps these delegates). A view that asks for the
 * keyboard hands its InputConnection to the system keyboard, which the
 * headset shows over the panel. On the desktop alone the PC keyboard types and
 * no soft keyboard opens, as on a device with a hardware keyboard attached.
 */
public class InputMethodManager_Delegate {
  private InputMethodManager_Delegate() {}

  static boolean isInEditMode() { return false; }

  static boolean showSoftInput(InputMethodManager manager, View view, int flags) {
    return show(view, null);
  }

  static boolean showSoftInput(InputMethodManager manager, View view, int flags, ResultReceiver result) {
    return show(view, result);
  }

  static boolean showSoftInput(InputMethodManager manager, View view, int flags, ResultReceiver result, int reason) {
    return show(view, result);
  }

  static boolean showSoftInput(InputMethodManager manager, View view, ImeTracker.Token token, int flags,
      ResultReceiver result, int reason) {
    return show(view, result);
  }

  static boolean hideSoftInputFromWindow(InputMethodManager manager, IBinder token, int flags) {
    return hide(null);
  }

  static boolean hideSoftInputFromWindow(InputMethodManager manager, IBinder token, int flags, ResultReceiver result) {
    return hide(result);
  }

  static boolean hideSoftInputFromWindow(InputMethodManager manager, IBinder token, int flags, ResultReceiver result,
      int reason, ImeTracker.Token statsToken) {
    return hide(result);
  }

  private static boolean show(View view, ResultReceiver result) {
    boolean was = SystemKeyboard.isShown();
    boolean headset = PanelHost.inHeadset();
    boolean shown = headset && SystemKeyboard.show(view);
    android.util.Log.d("InputMethod", "showSoftInput for " + (view != null ? view.getClass().getName() : "no view")
        + (shown ? ": the system keyboard" : headset ? ": no text to edit" : ": the desktop's keyboard types"));
    if (result != null) {
      result.send(shown ? (was ? InputMethodManager.RESULT_UNCHANGED_SHOWN : InputMethodManager.RESULT_SHOWN)
          : InputMethodManager.RESULT_UNCHANGED_HIDDEN, null);
    }
    return shown;
  }

  private static boolean hide(ResultReceiver result) {
    boolean hidden = SystemKeyboard.hide();
    if (result != null) {
      result.send(hidden ? InputMethodManager.RESULT_HIDDEN : InputMethodManager.RESULT_UNCHANGED_HIDDEN, null);
    }
    return hidden;
  }
}
