package android.database;
public abstract class ContentObserver {
  private final android.os.Handler handler;
  public ContentObserver(android.os.Handler h) {
    handler = h;
  }
  public boolean deliverSelfNotifications() {
    return false;
  }
  public void onChange(boolean selfChange) {}
  public void onChange(boolean selfChange, android.net.Uri uri) {
    onChange(selfChange);
  }
  public final void dispatchChange(boolean selfChange, android.net.Uri uri) {
    Runnable r = () -> onChange(selfChange, uri);
    if (handler == null)
      r.run();
    else
      handler.post(r);
  }
}
