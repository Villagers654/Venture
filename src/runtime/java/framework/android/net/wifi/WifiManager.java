package android.net.wifi;

// A Quest's Wi-Fi radio is always enabled. Locks only count holds: the host
// owns its network adapter's power and multicast filtering.
public class WifiManager {
  public static final int WIFI_STATE_DISABLED = 1, WIFI_STATE_ENABLED = 3;
  public static final int WIFI_MODE_FULL = 1, WIFI_MODE_FULL_HIGH_PERF = 3, WIFI_MODE_FULL_LOW_LATENCY = 4;
  public boolean isWifiEnabled() { return true; }
  public int getWifiState() { return WIFI_STATE_ENABLED; }
  public MulticastLock createMulticastLock(String tag) { return new MulticastLock(tag); }
  public WifiLock createWifiLock(int mode, String tag) { return new WifiLock(tag); }
  public WifiLock createWifiLock(String tag) { return new WifiLock(tag); }
  // AOSP's reference-counted holds: each acquire needs a release unless counting is off.
  public static class MulticastLock {
    private final String tag;
    private int count;
    private boolean counted = true;
    MulticastLock(String tag) { this.tag = tag; }
    public synchronized void acquire() { count = counted ? count + 1 : 1; }
    public synchronized void release() {
      if (count == 0) throw new RuntimeException("MulticastLock under-locked " + tag);
      count = counted ? count - 1 : 0;
    }
    public synchronized boolean isHeld() { return count > 0; }
    public synchronized void setReferenceCounted(boolean refCounted) { counted = refCounted; }
    @Override public String toString() { return "MulticastLock{" + tag + "; held " + count + "}"; }
  }
  public static class WifiLock {
    private final String tag;
    private int count;
    private boolean counted = true;
    WifiLock(String tag) { this.tag = tag; }
    public synchronized void acquire() { count = counted ? count + 1 : 1; }
    public synchronized void release() {
      if (count == 0) throw new RuntimeException("WifiLock under-locked " + tag);
      count = counted ? count - 1 : 0;
    }
    public synchronized boolean isHeld() { return count > 0; }
    public synchronized void setReferenceCounted(boolean refCounted) { counted = refCounted; }
    @Override public String toString() { return "WifiLock{" + tag + "; held " + count + "}"; }
  }
}
