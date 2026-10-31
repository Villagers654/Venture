package android.app;

import android.os.Process;
import com.venture.runtime.RuntimeEnvironment;
import java.util.List;

/** The hosted application is the sole Android process in this runtime. */
public class ActivityManager {
  // AOSP frameworks/base/core/java/android/app/ActivityManager.java, API 32.
  public static class RunningAppProcessInfo {
    public static final int IMPORTANCE_FOREGROUND = 100, IMPORTANCE_BACKGROUND = 400;
    public int pid, uid, importance, flags, lastTrimLevel;
    public String processName;
    public String[] pkgList;
    public RunningAppProcessInfo() {}
    public RunningAppProcessInfo(String name, int pid, String[] packages) {
      this.processName = name;
      this.pid = pid;
      this.pkgList = packages;
    }
  }
  private static RunningAppProcessInfo currentProcess() {
    String name = RuntimeEnvironment.get().manifest.info.packageName;
    RunningAppProcessInfo info = new RunningAppProcessInfo(name, Process.myPid(), new String[] {name});
    info.uid = Process.myUid();
    info.importance = RunningAppProcessInfo.IMPORTANCE_FOREGROUND;
    return info;
  }
  public List<RunningAppProcessInfo> getRunningAppProcesses() {
    return List.of(currentProcess());
  }
  public static void getMyMemoryState(RunningAppProcessInfo out) {
    RunningAppProcessInfo current = currentProcess();
    out.pid = current.pid;
    out.uid = current.uid;
    out.processName = current.processName;
    out.pkgList = current.pkgList;
    out.importance = current.importance;
    out.flags = 0;
    out.lastTrimLevel = 0;
  }
  public static class MemoryInfo {
    public long advertisedMem, availMem, totalMem, threshold;
    public boolean lowMemory;
  }
  public void getMemoryInfo(MemoryInfo out) {
    // Same manifest-selected Quest profile as libc sysinfo and /proc/meminfo.
    out.totalMem = Long.getLong("venture.device.memory", 8L * 1024 * 1024 * 1024);
    out.advertisedMem = out.totalMem;
    out.availMem = out.totalMem * 3 / 4;
    // Venture's local pressure policy: the lower of 256 MiB and 5% of RAM.
    out.threshold = Math.min(256L * 1024 * 1024, out.totalMem / 20);
    out.lowMemory = out.availMem < out.threshold;
  }
  // No earlier process of this app recorded an exit with the system.
  public List<ApplicationExitInfo> getHistoricalProcessExitReasons(String packageName, int pid, int maxNum) {
    if (pid < 0) throw new IllegalArgumentException("Invalid pid: " + pid);
    if (maxNum < 0) throw new IllegalArgumentException("Invalid maxNum: " + maxNum);
    return List.of();
  }
  public int getMemoryClass() {
    return (int) Math.min(Integer.MAX_VALUE, Runtime.getRuntime().maxMemory() / (1024 * 1024));
  }
  public android.os.Debug.MemoryInfo[] getProcessMemoryInfo(int[] pids) {
    if (pids == null) throw new NullPointerException("pids");
    var result = new android.os.Debug.MemoryInfo[pids.length];
    for (int i = 0; i < pids.length; ++i) {
      if (pids[i] != Process.myPid()) throw new SecurityException("Only the hosted process can be queried");
      result[i] = new android.os.Debug.MemoryInfo();
      android.os.Debug.getMemoryInfo(result[i]);
    }
    return result;
  }
  public int getLargeMemoryClass() { return getMemoryClass(); }
  public boolean isLowRamDevice() {
    MemoryInfo memory = new MemoryInfo();
    getMemoryInfo(memory);
    return memory.totalMem <= 2L * 1024 * 1024 * 1024;
  }
  // Bugsnag reads these while starting (Supernatural); a Quest app is never
  // background-restricted, monkey-driven or under a test harness.
  public boolean isBackgroundRestricted() { return false; }
  public static boolean isUserAMonkey() { return false; }
  public static boolean isRunningInTestHarness() { return false; }
  public static boolean isRunningInUserTestHarness() { return false; }
}
