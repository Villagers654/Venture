package android.accounts;

/** AOSP android.accounts.NetworkErrorException: an authenticator could not reach its server. */
public class NetworkErrorException extends AccountsException {
  public NetworkErrorException() { super(); }
  public NetworkErrorException(String message) { super(message); }
  public NetworkErrorException(String message, Throwable cause) { super(message, cause); }
  public NetworkErrorException(Throwable cause) { super(cause); }
}
