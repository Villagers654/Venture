package android.webkit;

/** AOSP android.webkit.SslErrorHandler: continues or cancels a load after a TLS error. */
public class SslErrorHandler extends android.os.Handler {
  public SslErrorHandler() { super(android.os.Looper.getMainLooper()); }
  public void proceed() {}
  public void cancel() {}
}
