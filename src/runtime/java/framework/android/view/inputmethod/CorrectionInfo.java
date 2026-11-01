package android.view.inputmethod;
public final class CorrectionInfo {
  private final int offset;
  private final CharSequence oldText, newText;
  public CorrectionInfo(int offset, CharSequence oldText, CharSequence newText) {
    this.offset = offset; this.oldText = oldText; this.newText = newText;
  }
  public int getOffset() { return offset; }
  public CharSequence getOldText() { return oldText; }
  public CharSequence getNewText() { return newText; }
}
