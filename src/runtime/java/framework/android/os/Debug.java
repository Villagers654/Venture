package android.os;

import com.venture.runtime.RuntimeEnvironment;
import java.util.Map;

public final class Debug {
  public static boolean isDebuggerConnected() {
    // Guest code executes in this process: expose its native debugger state.
    return RuntimeEnvironment.host().debuggerConnected();
  }
  // AOSP frameworks/base/core/java/android/os/Debug.java: MemoryInfo uses KiB.
  // The hosted JVM is not Dalvik. Account unshared resident host pages in the
  // native bucket and proportional shared pages in other; do not invent a
  // Dalvik heap, swap residency, or the Android kernel's detailed categories.
  // The summary stats are AOSP's full key set (shared pages count as system);
  // The Climb reads them at start and stops on a missing one.
  public static class MemoryInfo {
    public int dalvikPss, nativePss, otherPss;
    public int getTotalPss() { return dalvikPss + nativePss + otherPss; }
    public int getTotalSwappedOutPss() { return 0; }
    public int getSummaryJavaHeap() { return dalvikPss; }
    public int getSummaryNativeHeap() { return nativePss; }
    public int getSummaryCode() { return 0; }
    public int getSummaryStack() { return 0; }
    public int getSummaryGraphics() { return 0; }
    public int getSummaryPrivateOther() { return 0; }
    public int getSummarySystem() { return otherPss; }
    public int getSummaryTotalPss() { return getTotalPss(); }
    public int getSummaryTotalSwap() { return 0; }
    public Map<String, String> getMemoryStats() {
      return Map.of("summary.java-heap", Integer.toString(getSummaryJavaHeap()),
          "summary.native-heap", Integer.toString(getSummaryNativeHeap()),
          "summary.code", Integer.toString(getSummaryCode()),
          "summary.stack", Integer.toString(getSummaryStack()),
          "summary.graphics", Integer.toString(getSummaryGraphics()),
          "summary.private-other", Integer.toString(getSummaryPrivateOther()),
          "summary.system", Integer.toString(getSummarySystem()),
          "summary.total-pss", Integer.toString(getSummaryTotalPss()),
          "summary.total-swap", Integer.toString(getSummaryTotalSwap()));
    }
    public String getMemoryStat(String name) { return getMemoryStats().get(name); }
  }
  // Native heap in bytes (AOSP: mallinfo). The guest heap shares the process's
  // unshared resident pages, the native bucket of getMemoryInfo; none is
  // reported free. The Climb sizes its memory pools from these at start.
  public static long getNativeHeapSize() {
    return getNativeHeapAllocatedSize();
  }
  public static long getNativeHeapAllocatedSize() {
    return RuntimeEnvironment.host().processMemory()[0] * 1024;
  }
  public static long getNativeHeapFreeSize() {
    return 0;
  }
  // Total proportional set size in KiB.
  public static long getPss() {
    long[] residency = RuntimeEnvironment.host().processMemory();
    return residency[0] + residency[1];
  }
  public static boolean waitingForDebugger() {
    return false;
  }
  public static long threadCpuTimeNanos() {
    return java.lang.management.ManagementFactory.getThreadMXBean().getCurrentThreadCpuTime();
  }
  public static void getMemoryInfo(MemoryInfo out) {
    if (out == null) throw new NullPointerException("out");
    long[] residency = RuntimeEnvironment.host().processMemory();
    out.dalvikPss = 0;
    out.nativePss = Math.toIntExact(residency[0]);
    out.otherPss = Math.toIntExact(residency[1]);
  }
}
