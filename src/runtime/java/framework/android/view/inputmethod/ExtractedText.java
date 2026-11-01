package android.view.inputmethod;
public class ExtractedText {
  public static final int FLAG_SINGLE_LINE = 1, FLAG_SELECTING = 2;
  public CharSequence text, hint;
  public int startOffset, partialStartOffset = -1, partialEndOffset, selectionStart, selectionEnd, flags;
}
