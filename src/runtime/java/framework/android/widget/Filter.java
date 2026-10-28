package android.widget;

import android.os.*;

/** Serial background filtering, with results delivered to the creating Looper. */
public abstract class Filter {
  private final Handler handler =
      new Handler(Looper.myLooper() != null ? Looper.myLooper() : Looper.getMainLooper());
  private static final java.util.concurrent.ExecutorService worker =
      java.util.concurrent.Executors.newSingleThreadExecutor(r -> {
        Thread thread = new Thread(r, "Android-Filter");
        thread.setDaemon(true);
        return thread;
      });
  private final java.util.concurrent.atomic.AtomicLong generation =
      new java.util.concurrent.atomic.AtomicLong();
  public interface FilterListener {
    void onFilterComplete(int count);
  }
  protected static class FilterResults {
    public Object values;
    public int count;
  }
  public final void filter(CharSequence constraint) {
    filter(constraint, null);
  }
  public final void filter(CharSequence constraint, FilterListener listener) {
    String copy = constraint == null ? null : constraint.toString();
    long request = generation.incrementAndGet();
    worker.execute(() -> {
      if (request != generation.get())
        return;
      FilterResults results = performFiltering(copy);
      handler.post(() -> {
        if (request != generation.get())
          return;
        publishResults(copy, results);
        if (listener != null)
          listener.onFilterComplete(results.count);
      });
    });
  }
  protected abstract FilterResults performFiltering(CharSequence constraint);
  protected abstract void publishResults(CharSequence constraint, FilterResults results);
  public CharSequence convertResultToString(Object result) {
    return result == null ? "" : result.toString();
  }
}
