package android.os;
public final class Process {
  public static final int THREAD_PRIORITY_AUDIO = -16;
  public static final int THREAD_PRIORITY_URGENT_AUDIO = -19;
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
