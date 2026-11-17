package android.hardware.biometrics;

/** Prompts end at once with BIOMETRIC_ERROR_HW_NOT_PRESENT: there is no sensor or credential to ask for. */
public class BiometricPrompt {
  public static final int BIOMETRIC_ERROR_HW_UNAVAILABLE = 1, BIOMETRIC_ERROR_HW_NOT_PRESENT = 12,
                          AUTHENTICATION_RESULT_TYPE_BIOMETRIC = 2, AUTHENTICATION_RESULT_TYPE_DEVICE_CREDENTIAL = 1;
  public static class CryptoObject {
    private final Object crypto;
    public CryptoObject(java.security.Signature signature) { crypto = signature; }
    public CryptoObject(javax.crypto.Cipher cipher) { crypto = cipher; }
    public CryptoObject(javax.crypto.Mac mac) { crypto = mac; }
    public CryptoObject(android.security.identity.IdentityCredential credential) { crypto = credential; }
    public java.security.Signature getSignature() { return crypto instanceof java.security.Signature s ? s : null; }
    public javax.crypto.Cipher getCipher() { return crypto instanceof javax.crypto.Cipher c ? c : null; }
    public javax.crypto.Mac getMac() { return crypto instanceof javax.crypto.Mac m ? m : null; }
    public android.security.identity.IdentityCredential getIdentityCredential() {
      return crypto instanceof android.security.identity.IdentityCredential i ? i : null;
    }
  }
  public static class AuthenticationResult {
    private final CryptoObject crypto;
    private final int type;
    public AuthenticationResult(CryptoObject crypto, int type) { this.crypto = crypto; this.type = type; }
    public CryptoObject getCryptoObject() { return crypto; }
    public int getAuthenticationType() { return type; }
  }
  public abstract static class AuthenticationCallback {
    public AuthenticationCallback() {}
    public void onAuthenticationError(int errorCode, CharSequence errString) {}
    public void onAuthenticationHelp(int helpCode, CharSequence helpString) {}
    public void onAuthenticationSucceeded(AuthenticationResult result) {}
    public void onAuthenticationFailed() {}
  }
  public static class Builder {
    public Builder(android.content.Context context) {}
    public Builder setTitle(CharSequence title) { return this; }
    public Builder setSubtitle(CharSequence subtitle) { return this; }
    public Builder setDescription(CharSequence description) { return this; }
    public Builder setConfirmationRequired(boolean required) { return this; }
    @Deprecated public Builder setDeviceCredentialAllowed(boolean allowed) { return this; }
    public Builder setAllowedAuthenticators(int authenticators) { return this; }
    public Builder setNegativeButton(CharSequence text, java.util.concurrent.Executor executor,
        android.content.DialogInterface.OnClickListener listener) { return this; }
    public BiometricPrompt build() { return new BiometricPrompt(); }
  }
  public void authenticate(android.os.CancellationSignal cancel, java.util.concurrent.Executor executor,
      AuthenticationCallback callback) {
    executor.execute(() -> callback.onAuthenticationError(BIOMETRIC_ERROR_HW_NOT_PRESENT, "No biometric hardware"));
  }
  public void authenticate(CryptoObject crypto, android.os.CancellationSignal cancel,
      java.util.concurrent.Executor executor, AuthenticationCallback callback) {
    authenticate(cancel, executor, callback);
  }
}
