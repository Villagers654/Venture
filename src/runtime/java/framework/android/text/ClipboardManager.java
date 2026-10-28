package android.text;
public class ClipboardManager {
  protected CharSequence text;
  public synchronized void setText(CharSequence t) {
    text = t;
  }
  public synchronized CharSequence getText() {
    return text;
  }
  public synchronized boolean hasText() {
    return text != null && text.length() > 0;
  }
}
