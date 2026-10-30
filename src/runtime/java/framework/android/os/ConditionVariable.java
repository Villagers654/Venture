package android.os;

/** AOSP android.os.ConditionVariable: a gate threads block on until it is opened. */
public class ConditionVariable {
  private volatile boolean condition;
  public ConditionVariable() { condition = false; }
  public ConditionVariable(boolean state) { condition = state; }
  public void open() {
    synchronized (this) {
      boolean old = condition;
      condition = true;
      if (!old) notifyAll();
    }
  }
  public void close() {
    synchronized (this) { condition = false; }
  }
  public void block() {
    synchronized (this) {
      while (!condition) {
        try { wait(); } catch (InterruptedException e) {}
      }
    }
  }
  public boolean block(long timeoutMs) {
    if (timeoutMs == 0) { block(); return true; }
    synchronized (this) {
      long now = SystemClock.elapsedRealtime(), end = now + timeoutMs;
      while (!condition && now < end) {
        try { wait(end - now); } catch (InterruptedException e) {}
        now = SystemClock.elapsedRealtime();
      }
      return condition;
    }
  }
}
