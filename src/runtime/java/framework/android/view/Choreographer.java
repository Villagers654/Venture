package android.view;
import android.os.*;
import com.venture.runtime.RuntimeEnvironment;
import java.lang.ref.WeakReference;
import java.util.*;
import java.util.concurrent.CopyOnWriteArrayList;

public final class Choreographer {
  public interface FrameCallback {
    void doFrame(long frameTimeNanos);
  }
  private record Request(FrameCallback callback, long deadline) {}
  /** Last XR pulse: System.nanoTime of arrival, its display frame time and period. */
  private record Pulse(long arrival, long frameTime, long period) {}
  private static final List<WeakReference<Choreographer>> INSTANCES = new CopyOnWriteArrayList<>();
  private static volatile Pulse hostPulse;
  private static final ThreadLocal<Choreographer> LOCAL =
      ThreadLocal.withInitial(Choreographer::new);
  private final Handler handler = new Handler();
  private final List<Request> pending = new ArrayList<>();
  // Guarded by this. AOSP Choreographer.mLastFrameTimeNanos, when the fallback
  // timer last served a frame, and the last pulse whose display interval such a
  // fallback frame had already served.
  private long lastFrameTimeNanos = Long.MIN_VALUE;
  private long lastFallbackNanos = Long.MIN_VALUE;
  private Pulse servedByFallback;
  private final Runnable timer = () -> {
    long now = System.nanoTime();
    if (!RuntimeEnvironment.get().hostVsync) {
      dispatchFrame(now, now, true);
      return;
    }
    // XR pacing may pause while the guest waits for an Android frame. Continue
    // the host's display grid, and its frame-time clock, once pulses stop.
    Pulse pulse = hostPulse;
    long periods = pulse == null ? 0 : (now - pulse.arrival) / pulse.period;
    if (pulse != null && periods >= fallbackDelay(pulse))
      dispatchFrame(pulse.frameTime + periods * pulse.period, now, true);
    else
      schedule();
  };
  private Choreographer() {
    INSTANCES.add(new WeakReference<>(this));
  }
  public static Choreographer getInstance() {
    return LOCAL.get();
  }
  public void postFrameCallback(FrameCallback callback) {
    postFrameCallbackDelayed(callback, 0);
  }
  public synchronized void postFrameCallbackDelayed(FrameCallback callback, long delay) {
    pending.add(new Request(
        Objects.requireNonNull(callback), SystemClock.uptimeMillis() + Math.max(0, delay)));
    schedule();
  }
  public synchronized void removeFrameCallback(FrameCallback callback) {
    pending.removeIf(r -> r.callback == callback);
    schedule();
  }
  private synchronized void schedule() {
    handler.removeCallbacks(timer);
    if (pending.isEmpty())
      return;
    long earliest = pending.stream().mapToLong(Request::deadline).min().orElseThrow() * 1_000_000L;
    long base = Math.max(System.nanoTime(), earliest), frame;
    if (RuntimeEnvironment.get().hostVsync) {
      Pulse pulse = hostPulse;
      if (pulse == null) return; // Explicit host-clock mode, before XR pacing starts.
      // The first grid point after `base`, and at least fallbackDelay periods after the pulse.
      base = Math.max(base, pulse.arrival + fallbackDelay(pulse) * pulse.period - 1);
      frame = pulse.arrival + ((base - pulse.arrival) / pulse.period + 1) * pulse.period;
    } else {
      long period = framePeriod();
      frame = (base / period + 1) * period;
    }
    handler.postAtTime(timer, (frame + 999_999) / 1_000_000);
  }
  /**
   * Periods without a served pulse before the fallback timer runs a frame: two
   * after a served pulse, leaving the next pulse a period of slack. A pulse whose
   * interval a fallback frame already served owes its callbacks the next display
   * interval, as AOSP runs callbacks posted during a frame at the next vsync.
   */
  private synchronized long fallbackDelay(Pulse pulse) {
    return pulse == servedByFallback ? 1 : 2;
  }
  private static long framePeriod() {
    Pulse pulse = hostPulse;
    if (RuntimeEnvironment.get().hostVsync && pulse != null) return pulse.period;
    return Math.max(1, Math.round(1_000_000_000d / RuntimeEnvironment.get().refreshRate));
  }
  /** `arrival` is the System.nanoTime at which the pulse or fallback timer fired. */
  private void dispatchFrame(long frameTime, long arrival, boolean fallback) {
    dispatchFrame(frameTime, arrival, fallback, null);
  }
  private void dispatchFrame(long frameTime, long arrival, boolean fallback, Pulse pulse) {
    List<Request> ready = new ArrayList<>();
    synchronized (this) {
      long period = framePeriod();
      // AOSP runs one doFrame per vsync. A host pulse within half a period of a
      // fallback frame belongs to the display interval that frame served.
      if (!fallback && lastFallbackNanos != Long.MIN_VALUE
          && Math.abs(arrival - lastFallbackNanos) < period / 2) {
        servedByFallback = pulse;
        schedule();
        return;
      }
      long now = SystemClock.uptimeMillis();
      pending.removeIf(r -> {
        if (r.deadline <= now) {
          ready.add(r);
          return true;
        }
        return false;
      });
      if (!ready.isEmpty()) {
        // AOSP Choreographer.doFrame never reports a frame time earlier than the
        // last one; a later host pulse may predict less display latency.
        if (frameTime <= lastFrameTimeNanos) frameTime = lastFrameTimeNanos + period;
        lastFrameTimeNanos = frameTime;
        if (fallback) lastFallbackNanos = arrival;
      }
    }
    for (Request r : ready) r.callback.doFrame(frameTime);
    schedule();
  }
  /** A mode query/event can arrive while XR pulses are paused. */
  public static void displayPeriodChanged(long period) {
    Pulse pulse = hostPulse;
    if (pulse != null && pulse.period != period)
      hostPulse = new Pulse(pulse.arrival, pulse.frameTime, period);
    for (var weak : INSTANCES) {
      Choreographer c = weak.get();
      if (c != null) c.handler.post(c::schedule);
    }
  }
  /** Host XR/display integration; callbacks execute on their owning Looper. */
  public static void dispatchHostVsync(long frameTime, long period) {
    if (period <= 0) throw new IllegalArgumentException("Invalid vsync period");
    RuntimeEnvironment.get().refreshRate = (float)(1_000_000_000d / period);
    RuntimeEnvironment.get().hostVsync = true;
    // AOSP frameworks/base/core/java/android/view/Choreographer.java receives
    // DisplayEventReceiver pulses independently of the renderer.
    Pulse pulse = new Pulse(System.nanoTime(), frameTime, period);
    hostPulse = pulse;
    dispatchHost(frameTime, pulse.arrival, pulse);
  }
  public static void dispatchHostVsync(long frameTime) {
    dispatchHost(frameTime, System.nanoTime(), null);
  }
  private static void dispatchHost(long frameTime, long arrival, Pulse pulse) {
    for (var weak : INSTANCES) {
      Choreographer c = weak.get();
      if (c == null) {
        INSTANCES.remove(weak);
        continue;
      }
      synchronized (c) {
        if (!c.pending.isEmpty())
          c.handler.post(() -> c.dispatchFrame(frameTime, arrival, false, pulse));
      }
    }
  }
}
