package android.view.inputmethod;
public interface InputConnection {
  CharSequence getTextBeforeCursor(int length, int flags);
  CharSequence getTextAfterCursor(int length, int flags);
  CharSequence getSelectedText(int flags);
  int getCursorCapsMode(int modes);
  boolean deleteSurroundingText(int before, int after);
  boolean setComposingText(CharSequence text, int newCursorPosition);
  boolean setComposingRegion(int start, int end);
  boolean finishComposingText();
  boolean commitText(CharSequence text, int newCursorPosition);
  boolean setSelection(int start, int end);
  boolean sendKeyEvent(android.view.KeyEvent event);
  boolean performEditorAction(int action);
  boolean beginBatchEdit();
  boolean endBatchEdit();
  boolean performPrivateCommand(String action, android.os.Bundle data);
  boolean requestCursorUpdates(int mode);
  android.os.Handler getHandler();
  void closeConnection();
}
