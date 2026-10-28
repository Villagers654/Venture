package android.app;

import java.util.Objects;
import java.util.concurrent.*;

/** Process-owned ordered background work, drained at Android lifecycle boundaries. */
public final class QueuedWork {
  private QueuedWork() {}
  private static final Object lock = new Object();
  private static int pending, finishing;
  private static volatile Thread workerThread;
  private static long failures, lastWriteCompletion;
  private static boolean completedWrite;
  private static final ConcurrentLinkedQueue<Runnable> finishers = new ConcurrentLinkedQueue<>();
  private static final ExecutorService worker = Executors.newSingleThreadExecutor(task -> {
    Thread thread = new Thread(task, "QueuedWork");
    thread.setDaemon(true);
    workerThread = thread;
    return thread;
  });
  public static void addFinisher(Runnable finisher) {
    synchronized (lock) { finishers.add(Objects.requireNonNull(finisher)); lock.notifyAll(); }
  }
  public static void removeFinisher(Runnable finisher) {
    synchronized (lock) { finishers.remove(finisher); lock.notifyAll(); }
  }
  public static boolean hasPendingWork() {
    synchronized (lock) { return pending != 0; }
  }
  /** Failure history is process-wide and never reset by a later successful write. */
  public static long getFailureCount() { synchronized (lock) { return failures; } }
  /** No work/finishers pending and the last write attempt completed at least quietMs ago.
   * With no writes yet, an idle queue is quiet. This reports timing, not success;
   * callers requiring durability must also check finishPendingWrites(). */
  public static boolean writesQuiet(long quietMs) {
    if (quietMs < 0) throw new IllegalArgumentException("Negative quiet interval");
    synchronized (lock) {
      return pending == 0 && finishing == 0 && finishers.isEmpty()
          && (!completedWrite || System.nanoTime() - lastWriteCompletion >= TimeUnit.MILLISECONDS.toNanos(quietMs));
    }
  }
  public static void queue(Runnable work, boolean shouldDelay) {
    Objects.requireNonNull(work);
    enqueue(() -> { work.run(); return true; }, false, false, new CompletableFuture<>());
  }
  /** Submit one ordered disk write; false results are retained in drain status. */
  public static CompletableFuture<Boolean> queueWrite(java.util.function.BooleanSupplier write) {
    return queueWrite(write, false);
  }
  /** Only synchronous commits may run inline when invoked by the worker itself. */
  public static CompletableFuture<Boolean> queueWrite(java.util.function.BooleanSupplier write, boolean synchronous) {
    Objects.requireNonNull(write);
    var result = new CompletableFuture<Boolean>();
    enqueue(write, true, synchronous, result);
    return result;
  }
  private static void enqueue(java.util.function.BooleanSupplier work, boolean write, boolean synchronous, CompletableFuture<Boolean> result) {
    Runnable run = () -> {
      boolean success = false;
      Throwable thrown = null;
      try { success = work.getAsBoolean(); }
      catch (RuntimeException | Error failure) { thrown = failure; throw failure; }
      finally {
        synchronized (lock) {
          if (!success) ++failures;
          if (write) { completedWrite = true; lastWriteCompletion = System.nanoTime(); }
        }
        try {
          if (thrown == null) result.complete(success);
          else result.completeExceptionally(thrown);
        } finally { synchronized (lock) { --pending; lock.notifyAll(); } }
      }
    };
    boolean inline = synchronous && Thread.currentThread() == workerThread;
    synchronized (lock) {
      ++pending;
      if (!inline) {
        try { worker.execute(run); }
        catch (RuntimeException | Error failure) {
          ++failures;
          --pending;
          lock.notifyAll();
          throw failure;
        }
      }
    }
    // A worker cannot wait for its own executor. Complete nested writes through
    // the same accounting path, without holding the queue-state lock during IO.
    if (inline) run.run();
  }
  public static void waitToFinish() { finishPendingWrites(); }
  /** Drain all accepted work; false means at least one write/work item has failed
   * during this process. The original void waitToFinish ABI remains available. */
  public static boolean finishPendingWrites() {
    // Lifecycle durability must not be skipped merely because quit interrupted
    // its caller. Restore the interrupt status once all accepted work finishes.
    boolean interrupted = false;
    try {
      for (;;) {
        Runnable finisher;
        synchronized (lock) {
          finisher = finishers.poll();
          if (finisher != null) ++finishing;
          else {
            if (pending == 0 && finishing == 0) return failures == 0;
            try { lock.wait(); } catch (InterruptedException stop) { interrupted = true; }
          }
        }
        if (finisher != null) {
          try { finisher.run(); }
          catch (RuntimeException | Error failure) {
            synchronized (lock) { ++failures; }
            throw failure;
          } finally { synchronized (lock) { --finishing; lock.notifyAll(); } }
        }
      }
    } finally { if (interrupted) Thread.currentThread().interrupt(); }
  }
}
