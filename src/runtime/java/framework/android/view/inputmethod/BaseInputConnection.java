package android.view.inputmethod;
public class BaseInputConnection implements InputConnection {
  private final android.view.View target;
  private final android.text.Editable text = new android.text.SpannableStringBuilder();
  private static final Object COMPOSING = new Object();
  public BaseInputConnection(android.view.View v, boolean fullEditor) {
    target = v;
    android.text.Selection.setSelection(text, 0);
  }
  public android.text.Editable getEditable() {
    return target instanceof android.widget.EditText e ? e.getText() : text;
  }
  private int cursor() {
    return Math.max(0, android.text.Selection.getSelectionEnd(getEditable()));
  }
  public CharSequence getTextBeforeCursor(int n, int flags) {
    int c = cursor();
    return getEditable().subSequence(Math.max(0, c - n), c);
  }
  public CharSequence getTextAfterCursor(int n, int flags) {
    int c = cursor();
    return getEditable().subSequence(c, Math.min(getEditable().length(), c + n));
  }
  public CharSequence getSelectedText(int flags) {
    int a = Math.max(0, android.text.Selection.getSelectionStart(getEditable())), b = cursor();
    return getEditable().subSequence(Math.min(a, b), Math.max(a, b));
  }
  public int getCursorCapsMode(int modes) {
    return cursor() == 0 ? modes : 0;
  }
  public boolean deleteSurroundingText(int before, int after) {
    int c = cursor(), start = Math.max(0, c - before);
    getEditable().delete(start, Math.min(getEditable().length(), c + after));
    return setSelection(start, start);
  }
  public boolean commitText(CharSequence value, int newCursor) {
    return replaceText(value, newCursor, false);
  }
  private boolean replaceText(CharSequence value, int newCursor, boolean composing) {
    var editable = getEditable();
    int c = editable.getSpanStart(COMPOSING), end = editable.getSpanEnd(COMPOSING);
    if (c < 0) {
      c = Math.max(0, android.text.Selection.getSelectionStart(editable));
      end = cursor();
    }
    if (c > end) {
      int temp = c;
      c = end;
      end = temp;
    }
    removeComposingSpans(editable);
    editable.replace(c, end, value);
    if (composing)
      editable.setSpan(COMPOSING, c, c + value.length(), 289);
    int next = Math.max(0,
        Math.min(getEditable().length(),
            newCursor > 0 ? c + value.length() + newCursor - 1 : c + newCursor));
    return setSelection(next, next);
  }
  public boolean setComposingText(CharSequence text, int pos) {
    return replaceText(text, pos, true);
  }
  public boolean setComposingRegion(int s, int e) {
    int start = Math.max(0, Math.min(getEditable().length(), Math.min(s, e)));
    int end = Math.max(0, Math.min(getEditable().length(), Math.max(s, e)));
    getEditable().setSpan(COMPOSING, start, end, 289);
    return true;
  }
  public boolean finishComposingText() {
    removeComposingSpans(getEditable());
    return true;
  }
  public boolean setSelection(int s, int e) {
    android.text.Selection.setSelection(getEditable(), s, e);
    return true;
  }
  public boolean sendKeyEvent(android.view.KeyEvent e) {
    return target.dispatchKeyEvent(e);
  }
  public boolean performEditorAction(int action) {
    return false;
  }
  public boolean beginBatchEdit() {
    return false;
  }
  public boolean endBatchEdit() {
    return false;
  }
  public boolean performPrivateCommand(String action, android.os.Bundle data) {
    return false;
  }
  public boolean requestCursorUpdates(int mode) {
    return false;
  }
  public android.os.Handler getHandler() {
    return target.getHandler();
  }
  public void closeConnection() {
    finishComposingText();
  }
  public static void removeComposingSpans(android.text.Spannable text) {
    text.removeSpan(COMPOSING);
  }
  public boolean commitCompletion(CompletionInfo text) { return false; }
  public boolean commitCorrection(CorrectionInfo correctionInfo) { return false; }
  public ExtractedText getExtractedText(ExtractedTextRequest request, int flags) { return null; }
  public boolean setImeConsumesInput(boolean imeConsumesInput) { return false; }
  // AOSP: code points before and after the selection, never splitting a surrogate pair.
  public boolean deleteSurroundingTextInCodePoints(int before, int after) {
    var text = getEditable();
    if (text == null) return false;
    int start = android.text.Selection.getSelectionStart(text), end = android.text.Selection.getSelectionEnd(text);
    if (start < 0 || end < 0) return false;
    if (start > end) { int t = start; start = end; end = t; }
    String s = text.toString();
    int from = start, to = end;
    for (int i = 0; i < before && from > 0; ++i) from = s.offsetByCodePoints(from, -1);
    for (int i = 0; i < after && to < s.length(); ++i) to = s.offsetByCodePoints(to, 1);
    return deleteSurroundingText(start - from, to - end);
  }
  public static int getComposingSpanStart(android.text.Spannable text) { return -1; }
  public static int getComposingSpanEnd(android.text.Spannable text) { return -1; }
}
