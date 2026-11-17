package android.hardware.fingerprint;

/**
 * A Quest has no fingerprint sensor, so getSystemService never returns this manager (as on a device
 * without FEATURE_FINGERPRINT); the classes exist for libraries that reference them.
 */
@Deprecated
public class FingerprintManager {
  public static final int FINGERPRINT_ERROR_HW_UNAVAILABLE = 1, FINGERPRINT_ERROR_HW_NOT_PRESENT = 12;
  public static class CryptoObject {
    private final Object crypto;
    public CryptoObject(java.security.Signature signature) { crypto = signature; }
    public CryptoObject(javax.crypto.Cipher cipher) { crypto = cipher; }
    public CryptoObject(javax.crypto.Mac mac) { crypto = mac; }
    public java.security.Signature getSignature() { return crypto instanceof java.security.Signature s ? s : null; }
    public javax.crypto.Cipher getCipher() { return crypto instanceof javax.crypto.Cipher c ? c : null; }
    public javax.crypto.Mac getMac() { return crypto instanceof javax.crypto.Mac m ? m : null; }
  }
  public static class AuthenticationResult {
    private final CryptoObject crypto;
    public AuthenticationResult(CryptoObject crypto) { this.crypto = crypto; }
    public CryptoObject getCryptoObject() { return crypto; }
  }
  public abstract static class AuthenticationCallback {
    public AuthenticationCallback() {}
    public void onAuthenticationError(int errorCode, CharSequence errString) {}
    public void onAuthenticationHelp(int helpCode, CharSequence helpString) {}
    public void onAuthenticationSucceeded(AuthenticationResult result) {}
    public void onAuthenticationFailed() {}
  }
  public boolean isHardwareDetected() { return false; }
  public boolean hasEnrolledFingerprints() { return false; }
  public void authenticate(CryptoObject crypto, android.os.CancellationSignal cancel, int flags,
      AuthenticationCallback callback, android.os.Handler handler) {
    callback.onAuthenticationError(FINGERPRINT_ERROR_HW_NOT_PRESENT, "Fingerprint hardware not available");
  }
}
