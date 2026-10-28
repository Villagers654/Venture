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
  public static class MemoryInfo {
    public int dalvikPss, nativePss, otherPss;
    public int getTotalPss() { return dalvikPss + nativePss + otherPss; }
    public Map<String, String> getMemoryStats() {
      return Map.of("summary.total-pss", Integer.toString(getTotalPss()));
    }
    public String getMemoryStat(String name) { return getMemoryStats().get(name); }
  }
  public static void getMemoryInfo(MemoryInfo out) {
    if (out == null) throw new NullPointerException("out");
    long[] residency = RuntimeEnvironment.host().processMemory();
    out.dalvikPss = 0;
    out.nativePss = Math.toIntExact(residency[0]);
    out.otherPss = Math.toIntExact(residency[1]);
  }
}
