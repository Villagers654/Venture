package android.app;
public class Dialog implements android.content.DialogInterface {
  private final android.content.Context context;
  private final android.view.Window window;
  private CharSequence title;
  private boolean cancelable = true, showing;
  private android.content.DialogInterface.OnCancelListener cancelListener;
  private android.content.DialogInterface.OnDismissListener dismissListener;
  private android.content.DialogInterface.OnShowListener showListener;
  private android.content.DialogInterface.OnKeyListener keyListener;
  public void setOnCancelListener(android.content.DialogInterface.OnCancelListener listener) { cancelListener = listener; }
  public void setOnDismissListener(android.content.DialogInterface.OnDismissListener listener) { dismissListener = listener; }
  public void setOnShowListener(android.content.DialogInterface.OnShowListener listener) { showListener = listener; }
  public void setOnKeyListener(android.content.DialogInterface.OnKeyListener listener) { keyListener = listener; }
  public boolean dispatchKeyEvent(android.view.KeyEvent event) {
    return keyListener != null && keyListener.onKey(this, event.getKeyCode(), event);
  }
  public Dialog(android.content.Context c) {
    context = c;
    window = new android.view.Window(c);
  }
  public Dialog(android.content.Context c, int theme) {
    this(c);
  }
  public android.content.Context getContext() {
    return context;
  }
  public android.view.Window getWindow() {
    return window;
  }
  public void setContentView(android.view.View view) { window.setContentView(view); }
  public void setContentView(int layout) {
    setContentView(android.view.LayoutInflater.from(context).inflate(layout, null, false));
  }
  public <T extends android.view.View> T findViewById(int id) { return window.findViewById(id); }
  public void setTitle(CharSequence t) {
    title = t;
  }
  public void setCancelable(boolean v) {
    cancelable = v;
  }
  public void setCanceledOnTouchOutside(boolean cancel) {
    // AOSP Dialog.java: enabling outside cancellation also makes it cancelable.
    if (cancel) cancelable = true;
    window.setCloseOnTouchOutside(cancel);
  }
  public boolean onTouchEvent(android.view.MotionEvent event) {
    if (cancelable && showing && window.shouldCloseOnTouch(context, event)) {
      cancel();
      return true;
    }
    return false;
  }
  public boolean isShowing() {
    return showing;
  }
  public void show() {
    throw new UnsupportedOperationException("Host dialog presentation unavailable: " + title);
  }
  // AOSP hides the decor view of a shown dialog; nothing is presented here.
  public void hide() {}
  public void cancel() {
    if (cancelable) {
      if (cancelListener != null) cancelListener.onCancel(this);
      dismiss();
    }
  }
  public void dismiss() {
    boolean wasShowing = showing;
    showing = false;
    if (wasShowing && dismissListener != null) dismissListener.onDismiss(this);
  }
}
