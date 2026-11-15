package android.security.keystore;

import java.security.ProviderException;

/** Thrown for a StrongBox-backed key; Venture has no StrongBox. */
public class StrongBoxUnavailableException extends ProviderException {
  public StrongBoxUnavailableException() {}
  public StrongBoxUnavailableException(String message) { super(message); }
  public StrongBoxUnavailableException(String message, Throwable cause) { super(message, cause); }
  public StrongBoxUnavailableException(Throwable cause) { super(cause); }
}
