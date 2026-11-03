package android.util;

/**
 * Android 11's public leak tracker for closeable resources. Apps run with StrictMode's
 * leaked-closable detection off, so an unclosed resource reports nothing, as on a Quest.
 */
public final class CloseGuard {
  public CloseGuard() {}

  public void open(String closeMethodName) {
    java.util.Objects.requireNonNull(closeMethodName, "closeMethodName");
  }

  public void close() {}

  public void warnIfOpen() {}
}
