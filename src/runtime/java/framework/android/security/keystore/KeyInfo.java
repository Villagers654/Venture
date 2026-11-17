package android.security.keystore;

/** API 23. Venture's AndroidKeyStore keys live in software: never inside secure hardware. */
public class KeyInfo implements java.security.spec.KeySpec {
  private final String alias;
  private final int size, purposes;
  public KeyInfo(String alias, int size, int purposes) { this.alias = alias; this.size = size; this.purposes = purposes; }
  public String getKeystoreAlias() { return alias; }
  public int getKeySize() { return size; }
  public int getPurposes() { return purposes; }
  public boolean isInsideSecureHardware() { return false; }
  public int getSecurityLevel() { return 0; }  // KeyProperties.SECURITY_LEVEL_SOFTWARE
  public int getOrigin() { return 1; }  // KeyProperties.ORIGIN_GENERATED
  public boolean isUserAuthenticationRequired() { return false; }
  public boolean isInvalidatedByBiometricEnrollment() { return false; }
  public boolean isTrustedUserPresenceRequired() { return false; }
  public boolean isUserConfirmationRequired() { return false; }
  public int getRemainingUsageCount() { return -1; }
}
