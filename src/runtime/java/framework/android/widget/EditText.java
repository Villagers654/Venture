package android.widget;
public class EditText extends TextView {
  public EditText(android.content.Context c) {
    super(c);
  }
  public EditText(android.content.Context c, android.util.AttributeSet a) {
    super(c, a);
  }
  public EditText(android.content.Context c, android.util.AttributeSet a, int style) {
    super(c, a, style);
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
