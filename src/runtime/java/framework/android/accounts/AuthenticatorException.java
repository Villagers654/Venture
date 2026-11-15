package android.accounts;

/** AOSP android.accounts.AuthenticatorException: an account authenticator failed to answer. */
public class AuthenticatorException extends AccountsException {
  public AuthenticatorException() { super(); }
  public AuthenticatorException(String message) { super(message); }
  public AuthenticatorException(String message, Throwable cause) { super(message, cause); }
  public AuthenticatorException(Throwable cause) { super(cause); }
}
