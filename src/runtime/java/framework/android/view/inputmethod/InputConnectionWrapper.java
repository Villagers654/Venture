package android.view.inputmethod;

// AOSP 12L InputConnectionWrapper: delegation preserves target results and failures.
public class InputConnectionWrapper implements InputConnection {
  private InputConnection target;
  private final boolean mutable;
  public InputConnectionWrapper(InputConnection target, boolean mutable) { this.target = target; this.mutable = mutable; }
  public void setTarget(InputConnection target) {
    if (this.target != null && !mutable) throw new SecurityException("not mutable");
    this.target = target;
  }
  @Override public CharSequence getTextBeforeCursor(int length, int flags) { if (length < 0) throw new IllegalArgumentException("length < 0"); return target.getTextBeforeCursor(length, flags); }
  @Override public CharSequence getTextAfterCursor(int length, int flags) { if (length < 0) throw new IllegalArgumentException("length < 0"); return target.getTextAfterCursor(length, flags); }
  @Override public CharSequence getSelectedText(int flags) { return target.getSelectedText(flags); }
  @Override public int getCursorCapsMode(int modes) { return target.getCursorCapsMode(modes); }
  @Override public boolean deleteSurroundingText(int before, int after) { return target.deleteSurroundingText(before, after); }
  @Override public boolean setComposingText(CharSequence text, int newCursorPosition) { return target.setComposingText(text, newCursorPosition); }
  @Override public boolean setComposingRegion(int start, int end) { return target.setComposingRegion(start, end); }
  @Override public boolean finishComposingText() { return target.finishComposingText(); }
  @Override public boolean commitText(CharSequence text, int newCursorPosition) { return target.commitText(text, newCursorPosition); }
  @Override public boolean setSelection(int start, int end) { return target.setSelection(start, end); }
  @Override public boolean sendKeyEvent(android.view.KeyEvent event) { return target.sendKeyEvent(event); }
  @Override public boolean performEditorAction(int action) { return target.performEditorAction(action); }
  @Override public boolean beginBatchEdit() { return target.beginBatchEdit(); }
  @Override public boolean endBatchEdit() { return target.endBatchEdit(); }
  @Override public boolean performPrivateCommand(String action, android.os.Bundle data) { return target.performPrivateCommand(action, data); }
  @Override public boolean requestCursorUpdates(int mode) { return target.requestCursorUpdates(mode); }
  @Override public android.os.Handler getHandler() { return target.getHandler(); }
  @Override public void closeConnection() { target.closeConnection(); }
}
