package android.accounts;

/** AOSP android.accounts.AccountsException: the base of the AccountManager failures. */
public class AccountsException extends Exception {
  public AccountsException() { super(); }
  public AccountsException(String message) { super(message); }
  public AccountsException(String message, Throwable cause) { super(message, cause); }
  public AccountsException(Throwable cause) { super(cause); }
}
