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
  @Override String summary() {
    var title = title() == null ? "" : title().toString();
    var text = message == null ? "" : message.toString();
    var both = title.isEmpty() ? text : text.isEmpty() ? title : title + ": " + text;
    return both.isEmpty() ? "(untitled)" : both.replace('\n', ' ');
  }
  // Nobody can see or press this dialog, and games may wait for its answer
  // (Unity's "Continue"/"Abort" graphics warning blocks its engine thread).
  // Venture answers once show() has returned, as a click arrives on a Quest:
  // the negative button, which declines (Abort, Cancel, No), else the neutral
  // one (Later), else the positive one (OK). A dialog without buttons only
  // shows information and stays up.
  @Override public void show() {
    if (isShowing()) return;
    super.show();
    android.widget.Button answer = null;
    for (int which : new int[] {BUTTON_NEGATIVE, BUTTON_NEUTRAL, BUTTON_POSITIVE})
      if (answer == null) answer = buttons.get(which);
    var labels = new StringBuilder();
    for (int which : new int[] {BUTTON_POSITIVE, BUTTON_NEUTRAL, BUTTON_NEGATIVE}) {
      var button = buttons.get(which);
      if (button != null) labels.append(labels.length() == 0 ? "" : ", ").append(button.getText());
    }
    if (answer == null) {
      android.util.Log.i("dialog", "No buttons; the dialog stays shown");
      return;
    }
    android.util.Log.w("dialog", "Buttons: " + labels + "; Venture answers \"" + answer.getText() + "\"");
    final var chosen = answer;
    var looper = android.os.Looper.myLooper() != null ? android.os.Looper.myLooper() : android.os.Looper.getMainLooper();
    if (looper == null) { chosen.performClick(); return; }
    new android.os.Handler(looper).post(() -> { if (isShowing()) chosen.performClick(); });
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
    // A list dialog presents nothing here (Dialog.show reports that); its items are kept.
    private CharSequence[] items;
    public Builder setItems(CharSequence[] items, android.content.DialogInterface.OnClickListener listener) {
      this.items = items;
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
