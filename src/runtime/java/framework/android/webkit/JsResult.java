package android.webkit;

/** AOSP android.webkit.JsResult: the answer to a JavaScript dialog shown by WebChromeClient. */
public class JsResult {
  public interface ResultReceiver {
    void onJsResultComplete(JsResult result);
  }
  private final ResultReceiver receiver;
  private boolean result;
  public JsResult(ResultReceiver receiver) { this.receiver = receiver; }
  public final void cancel() { result = false; wakeUp(); }
  public final void confirm() { result = true; wakeUp(); }
  public final boolean getResult() { return result; }
  private void wakeUp() { if (receiver != null) receiver.onJsResultComplete(this); }
}
