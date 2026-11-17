package android.hardware.biometrics;

/** A Quest has no biometric sensor and Venture presents no lock screen credential. */
public class BiometricManager {
  public static final int BIOMETRIC_SUCCESS = 0, BIOMETRIC_ERROR_HW_UNAVAILABLE = 1, BIOMETRIC_ERROR_NONE_ENROLLED = 11,
                          BIOMETRIC_ERROR_NO_HARDWARE = 12, BIOMETRIC_ERROR_SECURITY_UPDATE_REQUIRED = 15;
  public interface Authenticators {
    int BIOMETRIC_STRONG = 0x000f, BIOMETRIC_WEAK = 0x00ff, DEVICE_CREDENTIAL = 0x8000;
  }
  public int canAuthenticate() { return BIOMETRIC_ERROR_NO_HARDWARE; }
  public int canAuthenticate(int authenticators) {
    return authenticators == Authenticators.DEVICE_CREDENTIAL ? BIOMETRIC_ERROR_NONE_ENROLLED : BIOMETRIC_ERROR_NO_HARDWARE;
  }
}
