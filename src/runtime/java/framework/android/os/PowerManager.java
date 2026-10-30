package android.os;
public class PowerManager {
  public static final int PARTIAL_WAKE_LOCK = 1;
  public static final int THERMAL_STATUS_NONE = 0, THERMAL_STATUS_LIGHT = 1,
      THERMAL_STATUS_MODERATE = 2, THERMAL_STATUS_SEVERE = 3,
      THERMAL_STATUS_CRITICAL = 4, THERMAL_STATUS_EMERGENCY = 5, THERMAL_STATUS_SHUTDOWN = 6;
  public interface OnThermalStatusChangedListener {
    void onThermalStatusChanged(int status);
  }
  private final java.util.Set<OnThermalStatusChangedListener> thermalListeners = new java.util.HashSet<>();
  private static final java.util.concurrent.atomic.AtomicBoolean thermalWarning = new java.util.concurrent.atomic.AtomicBoolean();
  public int getCurrentThermalStatus() {
    // AOSP android12L ThermalManagerService starts at THROTTLING_NONE and keeps
    // that status if no thermal HAL connects. Venture has no Android skin
    // temperature HAL; this is its documented fallback, not a PC temperature reading.
    if (thermalWarning.compareAndSet(false, true))
      android.util.Log.w("PowerManager", "No Android thermal HAL; thermal status uses Android's NONE fallback");
    return THERMAL_STATUS_NONE;
  }
  // AOSP PowerManager returns NaN when thermal headroom estimation is unavailable.
  public float getThermalHeadroom(int forecastSeconds) {
    if (forecastSeconds < 0 || forecastSeconds > 60)
      throw new IllegalArgumentException("Thermal forecast must be between 0 and 60 seconds");
    return Float.NaN;
  }
  public void addThermalStatusListener(OnThermalStatusChangedListener listener) {
    addThermalStatusListener(new android.content.Context().getMainExecutor(), listener);
  }
  public void addThermalStatusListener(java.util.concurrent.Executor executor, OnThermalStatusChangedListener listener) {
    java.util.Objects.requireNonNull(executor);
    java.util.Objects.requireNonNull(listener);
    synchronized (thermalListeners) {
      if (!thermalListeners.add(listener)) throw new IllegalArgumentException("Listener already registered");
    }
    // AOSP PowerManager / ThermalManagerService registers even without a HAL
    // and posts the current status once, then only reports actual status changes.
    int status = getCurrentThermalStatus();
    try {
      new android.content.Context().getMainExecutor().execute(() -> {
        try { executor.execute(() -> listener.onThermalStatusChanged(status)); }
        catch (RuntimeException error) { android.util.Log.e("PowerManager", "Thermal status callback failed"); }
      });
    } catch (RuntimeException error) {
      synchronized (thermalListeners) { thermalListeners.remove(listener); }
      throw error;
    }
  }
  public void removeThermalStatusListener(OnThermalStatusChangedListener listener) {
    java.util.Objects.requireNonNull(listener);
    synchronized (thermalListeners) {
      if (!thermalListeners.remove(listener)) throw new IllegalArgumentException("Listener was not added");
    }
  }
  public boolean isSustainedPerformanceModeSupported() { return false; }
  public boolean isInteractive() {
    return true;
  }
  public boolean isScreenOn() {
    return true;
  }
  public boolean isPowerSaveMode() {
    return false;
  }
  public boolean isDeviceIdleMode() {
    return false;
  }
  public boolean isIgnoringBatteryOptimizations(String pkg) {
    return true;
  }
  public WakeLock newWakeLock(int flags, String tag) {
    return new WakeLock();
  }
  public static class WakeLock {
    private int count;
    private boolean referenceCounted = true;
    public synchronized void acquire() {
      count = referenceCounted ? count + 1 : 1;
    }
    public synchronized void release() {
      if (count == 0)
        throw new IllegalStateException("WakeLock under-locked");
      count = referenceCounted ? count - 1 : 0;
    }
    public synchronized boolean isHeld() {
      return count > 0;
    }
    public synchronized void setReferenceCounted(boolean r) {
      referenceCounted = r;
    }
  }
}
