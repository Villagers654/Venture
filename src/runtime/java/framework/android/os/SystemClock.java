package android.os;
public final class SystemClock {
  public static long uptimeMillis() {
    return System.nanoTime() / 1_000_000;
  }
  public static long elapsedRealtime() {
    return uptimeMillis();
  }
  public static long elapsedRealtimeNanos() {
    return System.nanoTime();
  }
  public static long currentThreadTimeMillis() {
    return java.lang.management.ManagementFactory.getThreadMXBean().getCurrentThreadCpuTime()
        / 1_000_000;
  }
  public static void sleep(long millis) {
    long deadline = uptimeMillis() + Math.max(0, millis);
    boolean interrupted = false;
    for (long left; (left = deadline - uptimeMillis()) > 0;) try {
        Thread.sleep(left);
      } catch (InterruptedException e) {
        interrupted = true;
      }
    if (interrupted)
      Thread.currentThread().interrupt();
  }
}
