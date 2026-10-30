package android.os;

/** AOSP android.os.BadParcelableException. */
public class BadParcelableException extends android.util.AndroidRuntimeException {
  public BadParcelableException(String msg) { super(msg); }
  public BadParcelableException(Exception cause) { super(cause); }
  public BadParcelableException(String msg, Throwable cause) { super(msg, cause); }
}
