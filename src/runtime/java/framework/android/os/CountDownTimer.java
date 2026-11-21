package android.os;

/**
 * AOSP android.os.CountDownTimer (API 32): onTick every interval on the creating
 * thread's Looper until the deadline, then onFinish. A tick that runs long skips
 * the intervals it overran instead of firing them late in a burst; cancel() and
 * start() may be called from the callbacks.
 */
public abstract class CountDownTimer {
  private static final int MSG = 1;
  private final long millisInFuture, countdownInterval;
  private long stopTimeInFuture;
  private boolean cancelled;
  // As in AOSP, constructing one on a thread without a Looper throws.
  private final Handler handler = new Handler() {
    @Override public void handleMessage(Message msg) {
      synchronized (CountDownTimer.this) {
        if (cancelled) return;
        long millisLeft = stopTimeInFuture - SystemClock.elapsedRealtime();
        if (millisLeft <= 0) {
          onFinish();
          return;
        }
        long tickStart = SystemClock.elapsedRealtime();
        onTick(millisLeft);
        long tickDuration = SystemClock.elapsedRealtime() - tickStart;
        long delay;
        if (millisLeft < countdownInterval) {
          delay = Math.max(0, millisLeft - tickDuration);
        } else {
          delay = countdownInterval - tickDuration;
          while (delay < 0) delay += countdownInterval;
        }
        sendMessageDelayed(obtainMessage(MSG), delay);
      }
    }
  };

  public CountDownTimer(long millisInFuture, long countDownInterval) {
    this.millisInFuture = millisInFuture;
    this.countdownInterval = countDownInterval;
  }
  public final synchronized void cancel() {
    cancelled = true;
    handler.removeMessages(MSG);
  }
  public final synchronized CountDownTimer start() {
    cancelled = false;
    if (millisInFuture <= 0) {
      onFinish();
      return this;
    }
    stopTimeInFuture = SystemClock.elapsedRealtime() + millisInFuture;
    handler.sendMessage(handler.obtainMessage(MSG));
    return this;
  }
  public abstract void onTick(long millisUntilFinished);
  public abstract void onFinish();
}
