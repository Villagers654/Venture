package android.net;

import java.net.DatagramSocket;
import java.net.Socket;

/**
 * AOSP android.net.TrafficStats. Socket tags only label traffic for the system's accounting, so
 * they are kept per thread and otherwise ignored; byte and packet counters report UNSUPPORTED,
 * which apps already handle for devices without kernel statistics.
 */
public class TrafficStats {
  public static final int UNSUPPORTED = -1;
  private static final ThreadLocal<Integer> TAG = ThreadLocal.withInitial(() -> -1);
  private static final ThreadLocal<Integer> UID = ThreadLocal.withInitial(() -> -1);

  public TrafficStats() {}
  public static void setThreadStatsTag(int tag) { TAG.set(tag); }
  public static int getAndSetThreadStatsTag(int tag) {
    int old = TAG.get();
    TAG.set(tag);
    return old;
  }
  public static int getThreadStatsTag() { return TAG.get(); }
  public static void clearThreadStatsTag() { TAG.set(-1); }
  public static void setThreadStatsUid(int uid) { UID.set(uid); }
  public static int getThreadStatsUid() { return UID.get(); }
  public static void clearThreadStatsUid() { UID.set(-1); }
  public static void tagSocket(Socket socket) {}
  public static void untagSocket(Socket socket) {}
  public static void tagDatagramSocket(DatagramSocket socket) {}
  public static void untagDatagramSocket(DatagramSocket socket) {}
  public static void tagFileDescriptor(java.io.FileDescriptor fd) {}
  public static void untagFileDescriptor(java.io.FileDescriptor fd) {}
  public static void incrementOperationCount(int operationCount) {}
  public static void incrementOperationCount(int tag, int operationCount) {}
  public static long getMobileTxPackets() { return UNSUPPORTED; }
  public static long getMobileRxPackets() { return UNSUPPORTED; }
  public static long getMobileTxBytes() { return UNSUPPORTED; }
  public static long getMobileRxBytes() { return UNSUPPORTED; }
  public static long getTotalTxPackets() { return UNSUPPORTED; }
  public static long getTotalRxPackets() { return UNSUPPORTED; }
  public static long getTotalTxBytes() { return UNSUPPORTED; }
  public static long getTotalRxBytes() { return UNSUPPORTED; }
  public static long getUidTxBytes(int uid) { return UNSUPPORTED; }
  public static long getUidRxBytes(int uid) { return UNSUPPORTED; }
  public static long getUidTxPackets(int uid) { return UNSUPPORTED; }
  public static long getUidRxPackets(int uid) { return UNSUPPORTED; }
  public static long getRxPackets(String iface) { return UNSUPPORTED; }
  public static long getTxPackets(String iface) { return UNSUPPORTED; }
  public static long getRxBytes(String iface) { return UNSUPPORTED; }
  public static long getTxBytes(String iface) { return UNSUPPORTED; }
}
