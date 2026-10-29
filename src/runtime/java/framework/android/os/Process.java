package android.os;
public final class Process {
  public static final int THREAD_PRIORITY_DEFAULT = 0;
  public static final int THREAD_PRIORITY_AUDIO = -16;
  private static final java.util.concurrent.ConcurrentHashMap<Integer, Integer> PRIORITIES =
      new java.util.concurrent.ConcurrentHashMap<>();
  public static final int THREAD_PRIORITY_URGENT_AUDIO = -19;
  public static final int THREAD_PRIORITY_LOWEST = 19;
  public static final int THREAD_PRIORITY_BACKGROUND = 10;
  public static final int THREAD_PRIORITY_FOREGROUND = -2;
  public static final int THREAD_PRIORITY_DISPLAY = -4;
  public static final int THREAD_PRIORITY_URGENT_DISPLAY = -8;
  public static final int THREAD_PRIORITY_MORE_FAVORABLE = -1;
  public static final int THREAD_PRIORITY_LESS_FAVORABLE = 1;
  // When this process started, on the SystemClock uptime/elapsed clock (both
  // are the same here). Sentry's launch timing reads it before the activity.
  private static final long START_UPTIME = SystemClock.uptimeMillis()
      - ProcessHandle.current().info().startInstant()
          .map(start -> Math.max(0L, java.time.Duration.between(start, java.time.Instant.now()).toMillis()))
          .orElse(0L);
  public static long getStartUptimeMillis() {
    return START_UPTIME;
  }
  public static long getStartElapsedRealtime() {
    return START_UPTIME;
  }
  public static long getStartRequestedUptimeMillis() {
    return START_UPTIME;
  }
  public static long getStartRequestedElapsedRealtime() {
    return START_UPTIME;
  }
  public static boolean is64Bit() {
    return true;
  }
  public static boolean isIsolated() {
    return false;
  }
  public static boolean isApplicationUid(int uid) {
    return uid % 100000 >= 10000 && uid % 100000 <= 19999;
  }
  public static String myProcessName() {
    return android.app.Application.getProcessName();
  }
  public static int myPid() {
    return (int) ProcessHandle.current().pid();
  }
  public static int myTid() {
    return com.venture.runtime.RuntimeEnvironment.host().guestTid();
  }
  // AOSP: milliseconds of CPU time consumed by this process (all threads).
  public static long getElapsedCpuTime() {
    return ProcessHandle.current().info().totalCpuDuration().map(java.time.Duration::toMillis).orElse(0L);
  }
  public static int myUid() {
    return 10000;
  }
  public static void setThreadPriority(int p) {
    setThreadPriority(0, p);
  }
  public static void setThreadPriority(int tid, int p) {
    com.venture.runtime.RuntimeEnvironment.host().setThreadPriority(tid, p);
    PRIORITIES.put(tid == 0 ? myTid() : tid, p);
  }
  // The nice value last set through this class; threads start at THREAD_PRIORITY_DEFAULT.
  public static int getThreadPriority(int tid) {
    return PRIORITIES.getOrDefault(tid == 0 ? myTid() : tid, THREAD_PRIORITY_DEFAULT);
  }
  public static void killProcess(int pid) {
    if (pid != myPid())
      throw new SecurityException("Only this process may be terminated");
    android.util.Log.i("Process", "Application requested process termination");
    // Let the process owner drain guest workers, audio and the embedded JVM.
    // System.exit here would bypass that native runtime teardown.
    com.venture.runtime.AppMain.requestShutdown();
    com.venture.runtime.RuntimeEnvironment.host().exitProcess(com.venture.runtime.AppMain.exitStatus());
    throw new ThreadDeath();
  }
}
