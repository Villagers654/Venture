package android.os;

import com.venture.runtime.NativeHost;
import com.venture.runtime.RuntimeEnvironment;
import java.util.concurrent.ConcurrentHashMap;

/**
 * The natives of AOSP's MessageQueue in a 2D process (layoutlib-rules.txt:
 * delegate-natives). A queue waits on Venture's host looper, which also serves
 * the guest's ALoopers on that thread, as the classic framework's queue does;
 * without one (tests, a host without native loopers) it waits on a monitor.
 */
public final class MessageQueue_Delegate {
  private MessageQueue_Delegate() {}

  private static final class Waiter {
    boolean woken;
    boolean polling;
  }
  private static final ConcurrentHashMap<Long, Waiter> waiters = new ConcurrentHashMap<>();
  private static long nextWaiter = 1;
  // Host looper handles are addresses; monitor waiters are small negative ids.
  private static boolean hostLoopers() {
    return RuntimeEnvironment.host() instanceof NativeHost && NativeHost.hasNativeLoopers();
  }

  static long nativeInit() {
    if (hostLoopers()) return NativeHost.prepareLooper();
    synchronized (waiters) {
      long id = -(nextWaiter++);
      waiters.put(id, new Waiter());
      return id;
    }
  }

  static void nativeDestroy(long ptr) {
    if (ptr < 0) waiters.remove(ptr);
  }

  static void nativePollOnce(MessageQueue queue, long ptr, int timeoutMillis) {
    if (ptr > 0) {
      NativeHost.pollLooper(ptr, timeoutMillis);
      return;
    }
    Waiter waiter = waiters.get(ptr);
    if (waiter == null) return;
    synchronized (waiter) {
      if (timeoutMillis != 0 && !waiter.woken) {
        waiter.polling = true;
        long deadline = timeoutMillis < 0 ? Long.MAX_VALUE : System.nanoTime() + timeoutMillis * 1_000_000L;
        try {
          while (!waiter.woken) {
            long left = deadline - System.nanoTime();
            if (left <= 0) break;
            if (timeoutMillis < 0) waiter.wait();
            else waiter.wait(Math.max(1, left / 1_000_000L));
          }
        } catch (InterruptedException interrupted) {
          Thread.currentThread().interrupt();
        } finally {
          waiter.polling = false;
        }
      }
      waiter.woken = false;
    }
  }

  static void nativeWake(long ptr) {
    if (ptr > 0) {
      NativeHost.wakeLooper(ptr);
      return;
    }
    Waiter waiter = waiters.get(ptr);
    if (waiter == null) return;
    synchronized (waiter) {
      waiter.woken = true;
      waiter.notifyAll();
    }
  }

  static boolean nativeIsPolling(long ptr) {
    Waiter waiter = ptr < 0 ? waiters.get(ptr) : null;
    if (waiter == null) return false;
    synchronized (waiter) {
      return waiter.polling;
    }
  }

  // File descriptor events are for the system server's sockets; an app's are
  // served by its guest ALooper.
  static void nativeSetFileDescriptorEvents(long ptr, int fd, int events) {}

  static void nativeSetSkipEpollWaitForZeroTimeout(long ptr) {}
}
