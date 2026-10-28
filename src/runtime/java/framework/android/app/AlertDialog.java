package android.app;
public class AlertDialog extends Dialog {
  private CharSequence message;
  private final java.util.Map<Integer, android.widget.Button> buttons = new java.util.HashMap<>();
  public void setView(android.view.View view) { setContentView(view); }
  public void setButton(int which, CharSequence text, android.content.DialogInterface.OnClickListener listener) {
    if (which < -3 || which > -1) throw new IllegalArgumentException("Dialog button identifier");
    var button = new android.widget.Button(getContext());
    button.setText(text);
    button.setOnClickListener(view -> { if (listener != null) listener.onClick(this, which); dismiss(); });
    buttons.put(which, button);
  }
  public android.widget.Button getButton(int which) { return buttons.get(which); }
  public void setMessage(CharSequence value) { message = value; }
  @Override public void show() {
    android.util.Log.e("AlertDialog", String.valueOf(message));
    super.show();
  }
  public AlertDialog(android.content.Context c) {
    super(c);
  }
  public static class Builder {
    private final AlertDialog dialog;
    public Builder(android.content.Context c) {
      dialog = new AlertDialog(c);
    }
    public Builder setView(android.view.View view) { dialog.setView(view); return this; }
    public Builder setView(int layout) { dialog.setContentView(layout); return this; }
    public Builder setTitle(CharSequence t) {
      dialog.setTitle(t);
      return this;
    }
    public Builder setMessage(CharSequence t) {
      dialog.setMessage(t);
      return this;
    }
    public Builder setCancelable(boolean b) {
      dialog.setCancelable(b);
      return this;
    }
    public Builder setOnCancelListener(android.content.DialogInterface.OnCancelListener listener) {
      dialog.setOnCancelListener(listener);
      return this;
    }
    public Builder setOnDismissListener(android.content.DialogInterface.OnDismissListener listener) {
      dialog.setOnDismissListener(listener);
      return this;
    }
    public Builder setOnKeyListener(android.content.DialogInterface.OnKeyListener listener) {
      dialog.setOnKeyListener(listener);
      return this;
    }
    public Builder setPositiveButton(
        CharSequence text, android.content.DialogInterface.OnClickListener listener) {
      dialog.setButton(-1, text, listener);
      return this;
    }
    public Builder setNegativeButton(
        CharSequence text, android.content.DialogInterface.OnClickListener listener) {
      dialog.setButton(-2, text, listener);
      return this;
    }
    public Builder setNeutralButton(CharSequence text, android.content.DialogInterface.OnClickListener listener) {
      dialog.setButton(-3, text, listener); return this;
    }
    public AlertDialog create() {
      return dialog;
    }
    public AlertDialog show() {
      dialog.show();
      return dialog;
    }
  }
}
