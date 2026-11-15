package android.accounts;

/** AOSP android.accounts.OperationCanceledException: an AccountManager request was canceled. */
public class OperationCanceledException extends AccountsException {
  public OperationCanceledException() { super(); }
  public OperationCanceledException(String message) { super(message); }
  public OperationCanceledException(String message, Throwable cause) { super(message, cause); }
  public OperationCanceledException(Throwable cause) { super(cause); }
}
