package android.os;

import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

/** Background work with progress/completion dispatched to the Android main Looper. */
public abstract class AsyncTask<Params, Progress, Result> {
  public enum Status { PENDING, RUNNING, FINISHED }
  private static final ThreadFactory factory = runnable -> {
    Thread thread = new Thread(runnable, "Android-AsyncTask"); thread.setDaemon(true); return thread;
  };
  public static final Executor THREAD_POOL_EXECUTOR = Executors.newFixedThreadPool(
      Math.max(2, Math.min(4, Runtime.getRuntime().availableProcessors())), factory);
  public static final Executor SERIAL_EXECUTOR = Executors.newSingleThreadExecutor(factory);
  private static volatile Executor defaultExecutor = SERIAL_EXECUTOR;
  private final AtomicReference<Status> status = new AtomicReference<>(Status.PENDING);
  private final AtomicBoolean invoked = new AtomicBoolean(), delivered = new AtomicBoolean();
  private final Handler handler = new Handler(Looper.getMainLooper());
  private Params[] parameters;
  private final FutureTask<Result> task = new FutureTask<>(() -> {
    invoked.set(true);
    Result result = null;
    boolean failed = false;
    try { result = doInBackground(parameters); return result; }
    catch (RuntimeException | Error failure) { failed = true; throw failure; }
    finally {
      Result value = result;
      if (!failed) handler.post(() -> finish(value));
    }
  }) {
    protected void done() {
      try { get(); }
      catch (CancellationException cancelled) {
        if (!invoked.get()) handler.post(() -> finish(null));
      } catch (ExecutionException failure) {
        handler.post(() -> { status.set(Status.FINISHED); throw new RuntimeException("AsyncTask background failure", failure.getCause()); });
      } catch (InterruptedException interrupted) { Thread.currentThread().interrupt(); }
    }
  };
  protected abstract Result doInBackground(Params... parameters);
  protected void onPreExecute() {}
  protected void onPostExecute(Result result) {}
  protected void onProgressUpdate(Progress... progress) {}
  protected void onCancelled(Result result) { onCancelled(); }
  protected void onCancelled() {}
  public final Status getStatus() { return status.get(); }
  public final boolean isCancelled() { return task.isCancelled(); }
  public final boolean cancel(boolean interrupt) { return task.cancel(interrupt); }
  public final Result get() throws InterruptedException, ExecutionException { return task.get(); }
  public final Result get(long timeout, TimeUnit unit)
      throws InterruptedException, ExecutionException, TimeoutException { return task.get(timeout, unit); }
  @SafeVarargs public final AsyncTask<Params, Progress, Result> execute(Params... parameters) {
    return executeOnExecutor(defaultExecutor, parameters);
  }
  @SafeVarargs public final AsyncTask<Params, Progress, Result> executeOnExecutor(Executor executor, Params... parameters) {
    if (!status.compareAndSet(Status.PENDING, Status.RUNNING)) throw new IllegalStateException("Task already executed");
    this.parameters = parameters.clone();
    onPreExecute();
    executor.execute(task);
    return this;
  }
  @SafeVarargs protected final void publishProgress(Progress... values) {
    Progress[] copy = values.clone();
    handler.post(() -> { if (!isCancelled()) onProgressUpdate(copy); });
  }
  private void finish(Result result) {
    if (!delivered.compareAndSet(false, true)) return;
    try { if (isCancelled()) onCancelled(result); else onPostExecute(result); }
    finally { status.set(Status.FINISHED); }
  }
  public static void execute(Runnable runnable) { defaultExecutor.execute(runnable); }
  public static void setDefaultExecutor(Executor executor) { defaultExecutor = java.util.Objects.requireNonNull(executor); }
}
