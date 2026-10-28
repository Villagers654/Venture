package android.util;
public class AndroidRuntimeException extends RuntimeException {
  public AndroidRuntimeException() {}
  public AndroidRuntimeException(String message) {
    super(message);
  }
  public AndroidRuntimeException(Exception cause) {
    super(cause);
  }
}
