package android.content;

import android.os.Bundle;

public abstract class BroadcastReceiver {
  // AOSP BroadcastReceiver.PendingResult: the result state of the broadcast being delivered.
  // Broadcasts here are unordered, so results are kept but nobody reads them.
  public static class PendingResult {
    private int resultCode;
    private String resultData;
    private Bundle resultExtras;
    private boolean abort, finished;
    PendingResult() {}
    public final void setResultCode(int code) { resultCode = code; }
    public final int getResultCode() { return resultCode; }
    public final void setResultData(String data) { resultData = data; }
    public final String getResultData() { return resultData; }
    public final void setResultExtras(Bundle extras) { resultExtras = extras; }
    public final Bundle getResultExtras(boolean makeMap) {
      if (resultExtras == null && makeMap) resultExtras = new Bundle();
      return resultExtras;
    }
    public final void setResult(int code, String data, Bundle extras) {
      resultCode = code; resultData = data; resultExtras = extras;
    }
    public final boolean getAbortBroadcast() { return abort; }
    public final void abortBroadcast() { abort = true; }
    public final void clearAbortBroadcast() { abort = false; }
    public final void finish() {
      if (finished) throw new IllegalStateException("Broadcast already finished");
      finished = true;
    }
  }
  private PendingResult pending;

  public abstract void onReceive(Context context, Intent intent);
  // One delivery: onReceive with a pending result, which goAsync may take over.
  final void ventureDeliver(Context context, Intent intent) {
    pending = new PendingResult();
    try { onReceive(context, intent); } finally { pending = null; }
  }
  public final PendingResult goAsync() {
    PendingResult result = pending;
    pending = null;
    return result;
  }
  private PendingResult result() {
    if (pending == null) throw new IllegalStateException("Call while result is not pending");
    return pending;
  }
  public final void setResultCode(int code) { result().setResultCode(code); }
  public final int getResultCode() { return pending != null ? pending.getResultCode() : 0; }
  public final void setResultData(String data) { result().setResultData(data); }
  public final String getResultData() { return pending != null ? pending.getResultData() : null; }
  public final void setResultExtras(Bundle extras) { result().setResultExtras(extras); }
  public final Bundle getResultExtras(boolean makeMap) { return pending != null ? pending.getResultExtras(makeMap) : null; }
  public final void setResult(int code, String data, Bundle extras) { result().setResult(code, data, extras); }
  public final boolean getAbortBroadcast() { return pending != null && pending.getAbortBroadcast(); }
  public final void abortBroadcast() { result().abortBroadcast(); }
  public final void clearAbortBroadcast() { if (pending != null) pending.clearAbortBroadcast(); }
  public boolean isInitialStickyBroadcast() {
    return true;
  }
  public boolean isOrderedBroadcast() {
    return false;
  }
}
