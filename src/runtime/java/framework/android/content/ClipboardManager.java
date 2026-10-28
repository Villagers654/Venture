package android.content;
public class ClipboardManager extends android.text.ClipboardManager {
  private ClipData clip;
  public synchronized void setPrimaryClip(ClipData c) {
    clip = c;
    setText(c.getItemAt(0).getText());
  }
  public synchronized ClipData getPrimaryClip() {
    return clip;
  }
  public synchronized boolean hasPrimaryClip() {
    return clip != null;
  }
}
