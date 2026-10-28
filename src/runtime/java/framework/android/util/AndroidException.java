package android.util;
public class AndroidException extends Exception {
  public AndroidException() {}
  public AndroidException(String message) { super(message); }
  public AndroidException(String message, Throwable cause) { super(message, cause); }
  public AndroidException(Exception cause) { super(cause); }
}
