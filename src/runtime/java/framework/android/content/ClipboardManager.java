package android.content;
// One clipboard for the app, as Android's is one per device: every
// getSystemService(CLIPBOARD_SERVICE) instance shares it. Listeners hear each
// change (House of the Dead 4 registers one at start).
public class ClipboardManager extends android.text.ClipboardManager {
  public interface OnPrimaryClipChangedListener {
    void onPrimaryClipChanged();
  }
  private static final Object lock = new Object();
  private static ClipData clip;
  private static final java.util.List<OnPrimaryClipChangedListener> listeners = new java.util.ArrayList<>();
  public void setPrimaryClip(ClipData c) {
    if (c == null) throw new NullPointerException("clip");
    synchronized (lock) {
      clip = c;
    }
    super.setText(c.getItemAt(0).getText());
    changed();
  }
  public void clearPrimaryClip() {
    synchronized (lock) {
      clip = null;
    }
    super.setText(null);
    changed();
  }
  public ClipData getPrimaryClip() {
    synchronized (lock) {
      return clip;
    }
  }
  public boolean hasPrimaryClip() {
    synchronized (lock) {
      return clip != null;
    }
  }
  @Override
  public void setText(CharSequence t) {
    setPrimaryClip(ClipData.newPlainText(null, t));
  }
  @Override
  public CharSequence getText() {
    var c = getPrimaryClip();
    return c == null ? null : c.getItemAt(0).getText();
  }
  @Override
  public boolean hasText() {
    var t = getText();
    return t != null && t.length() > 0;
  }
  public void addPrimaryClipChangedListener(OnPrimaryClipChangedListener what) {
    synchronized (lock) {
      listeners.add(what);
    }
  }
  public void removePrimaryClipChangedListener(OnPrimaryClipChangedListener what) {
    synchronized (lock) {
      listeners.remove(what);
    }
  }
  private static void changed() {
    OnPrimaryClipChangedListener[] current;
    synchronized (lock) {
      current = listeners.toArray(new OnPrimaryClipChangedListener[0]);
    }
    for (var l : current) l.onPrimaryClipChanged();
  }
}
