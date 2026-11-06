package android.widget;
public class CheckedTextView extends TextView implements Checkable {
  private boolean checked;
  public CheckedTextView(android.content.Context c) {
    super(c);
  }
  public CheckedTextView(android.content.Context c, android.util.AttributeSet a) {
    super(c, a);
  }
  public CheckedTextView(android.content.Context c, android.util.AttributeSet a, int style) {
    super(c, a, style);
  }
  public CheckedTextView(android.content.Context c, android.util.AttributeSet a, int style, int res) {
    super(c, a, style);
  }
  public void setChecked(boolean value) {
    checked = value;
  }
  public boolean isChecked() {
    return checked;
  }
  public void toggle() {
    setChecked(!checked);
  }
}