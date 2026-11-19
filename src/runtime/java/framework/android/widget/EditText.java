package android.widget;
public class EditText extends TextView {
  // AOSP: an editable TextView defaults to multi-line text input.
  private static final int DEFAULT_INPUT = android.text.InputType.TYPE_CLASS_TEXT
      | android.text.InputType.TYPE_TEXT_FLAG_MULTI_LINE;
  public EditText(android.content.Context c) {
    super(c);
    setRawInputType(DEFAULT_INPUT);
  }
  public EditText(android.content.Context c, android.util.AttributeSet a) {
    super(c, a);
    setRawInputType(DEFAULT_INPUT);
  }
  public EditText(android.content.Context c, android.util.AttributeSet a, int style) {
    super(c, a, style);
    setRawInputType(DEFAULT_INPUT);
  }
  public android.text.Editable getText() {
    return (android.text.Editable) super.getText();
  }
  public void setSelection(int start, int end) {
    int oldStart = getSelectionStart(), oldEnd = getSelectionEnd();
    android.text.Selection.setSelection(getText(), start, end);
    if (oldStart != start || oldEnd != end) onSelectionChanged(start, end);
  }
  public void setSelection(int index) {
    setSelection(index, index);
  }
}
