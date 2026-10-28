package android.os;
public class HandlerThread extends Thread {
  private Looper looper;
  public HandlerThread(String name) {
    super(name);
  }
  public HandlerThread(String name, int priority) {
    super(name);
  }
  public void run() {
    Looper.prepare();
    synchronized (this) {
      looper = Looper.myLooper();
      notifyAll();
    }
    onLooperPrepared();
    Looper.loop();
  }
  protected void onLooperPrepared() {}
  public synchronized Looper getLooper() {
    while (isAlive() && looper == null) try {
        wait();
      } catch (InterruptedException e) {
        Thread.currentThread().interrupt();
        return null;
      }
    return looper;
  }
  public boolean quit() {
    Looper l = getLooper();
    if (l == null)
      return false;
    l.quit();
    return true;
  }
  public boolean quitSafely() {
    Looper l = getLooper();
    if (l == null)
      return false;
    l.quitSafely();
    return true;
  }
}
