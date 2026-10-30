package android.os;

/** AOSP android.os.CancellationSignal, within one process. */
public final class CancellationSignal {
  public interface OnCancelListener { void onCancel(); }

  private boolean canceled;
  private OnCancelListener listener;

  public synchronized boolean isCanceled() { return canceled; }
  public void throwIfCanceled() {
    if (isCanceled()) throw new OperationCanceledException();
  }
  public void cancel() {
    OnCancelListener l;
    synchronized (this) {
      if (canceled) return;
      canceled = true;
      l = listener;
    }
    if (l != null) l.onCancel();
  }
  public void setOnCancelListener(OnCancelListener listener) {
    synchronized (this) {
      if (this.listener == listener) return;
      this.listener = listener;
      if (!canceled || listener == null) return;
    }
    listener.onCancel();
  }
}
