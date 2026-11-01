package android.view.inputmethod;
public final class CompletionInfo {
  private final long id;
  private final int position;
  private final CharSequence text, label;
  public CompletionInfo(long id, int index, CharSequence text) { this(id, index, text, null); }
  public CompletionInfo(long id, int index, CharSequence text, CharSequence label) {
    this.id = id; position = index; this.text = text; this.label = label;
  }
  public long getId() { return id; }
  public int getPosition() { return position; }
  public CharSequence getText() { return text; }
  public CharSequence getLabel() { return label; }
}
