package android.os;

import java.util.*;
import com.venture.runtime.NativeHost;
import com.venture.runtime.RuntimeEnvironment;

public final class MessageQueue {
  private static final boolean TRACE = com.venture.runtime.HostEnvironment.getenv("VENTURE_TRACE_LOOPER") != null;
  private int traceCount;
  private void trace(String action, Message message, int timeout) {
    if (traceCount++ < 64)
      System.err.println("[LooperTrace] " + Thread.currentThread().getName() + " " + action
          + " count=" + messages.size() + " timeout=" + timeout + " what="
          + (message == null ? -1 : message.what));
  }
  public interface IdleHandler {
    boolean queueIdle();
  }
  private final PriorityQueue<Message> messages = new PriorityQueue<>(
      Comparator.comparingLong((Message m) -> m.when).thenComparingLong(m -> m.sequence));
  private final List<IdleHandler> idleHandlers = new ArrayList<>();
  private long sequence;
  private boolean quitting;
  private final long nativeLooper = RuntimeEnvironment.host() instanceof NativeHost && NativeHost.hasNativeLoopers()
      ? NativeHost.prepareLooper() : 0;
  private void wake() {
    notifyAll();
    if (nativeLooper != 0) NativeHost.wakeLooper(nativeLooper);
  }
  synchronized boolean enqueue(Message m, long when) {
    if (m.inUse)
      throw new IllegalStateException("Message already enqueued");
    if (quitting)
      return false;
    m.inUse = true;
    m.when = when;
    m.sequence = sequence++;
    messages.add(m);
    if (TRACE) trace("enqueue", m, 0);
    wake();
    return true;
  }
  Message next() {
    boolean ranIdle = false;
    while (true) {
      if (nativeLooper != 0) NativeHost.pollLooper(nativeLooper, 0);
      List<IdleHandler> idle = null;
      int nativeTimeout = 0;
      synchronized (this) {
        if (quitting && messages.isEmpty())
          return null;
        Message first = messages.peek();
        long now = SystemClock.uptimeMillis();
        if (first != null && first.when <= now) {
          messages.poll();
          first.inUse = false;
          return first;
        }
        if (!ranIdle && !idleHandlers.isEmpty()) {
          idle = new ArrayList<>(idleHandlers);
          ranIdle = true;
        } else if (nativeLooper != 0) {
          nativeTimeout = first == null ? -1 : (int)Math.min(Integer.MAX_VALUE, Math.max(1, first.when - now));
          if (TRACE) trace("native-wait", first, nativeTimeout);
        } else
          try {
            wait(first == null ? 0 : Math.max(1, first.when - now));
          } catch (InterruptedException e) { /* wake and recheck */
          }
      }
      if (nativeLooper != 0 && idle == null) NativeHost.pollLooper(nativeLooper, nativeTimeout);
      if (idle != null)
        for (IdleHandler h : idle)
          if (!h.queueIdle())
            removeIdleHandler(h);
    }
  }
  synchronized void quit(boolean safe) {
    quitting = true;
    long now = SystemClock.uptimeMillis();
    messages.removeIf(m -> !safe || m.when > now);
    wake();
  }
  synchronized void remove(Handler h, Runnable r, Object token, Integer what) {
    messages.removeIf(m
        -> m.target == h && (r == null || m.callback == r) && (token == null || m.obj == token)
            && (what == null || m.what == what));
  }
  synchronized boolean has(Handler h, int what, Object object) {
    return messages.stream().anyMatch(
        m -> m.target == h && m.what == what && (object == null || m.obj == object));
  }
  public synchronized void addIdleHandler(IdleHandler h) {
    idleHandlers.add(Objects.requireNonNull(h));
    wake();
  }
  public synchronized void removeIdleHandler(IdleHandler h) {
    idleHandlers.remove(h);
  }
  public synchronized boolean isIdle() {
    return messages.isEmpty() || messages.peek().when > SystemClock.uptimeMillis();
  }
}
