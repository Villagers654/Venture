package android.security.keystore;

import java.security.KeyStore;
import java.util.Date;

/** How a key imported with KeyStore.setEntry into the Android Keystore may be used (API 32). */
public final class KeyProtection implements KeyStore.ProtectionParameter {
  private final int purposes;
  private final Date validityStart, originationEnd, consumptionEnd;
  private final String[] digests, encryptionPaddings, signaturePaddings, blockModes;
  private final boolean randomizedEncryption, userAuthentication, userPresence, userConfirmation,
      validWhileOnBody, invalidatedByBiometricEnrollment, strongBox, unlockedDevice;
  private final int authValiditySeconds, authType, maxUsageCount;

  private KeyProtection(Builder b) {
    purposes = b.purposes; validityStart = copy(b.validityStart);
    originationEnd = copy(b.originationEnd); consumptionEnd = copy(b.consumptionEnd);
    digests = copy(b.digests); encryptionPaddings = copy(b.encryptionPaddings);
    signaturePaddings = copy(b.signaturePaddings); blockModes = copy(b.blockModes);
    randomizedEncryption = b.randomizedEncryption; userAuthentication = b.userAuthentication;
    userPresence = b.userPresence; userConfirmation = b.userConfirmation;
    validWhileOnBody = b.validWhileOnBody;
    invalidatedByBiometricEnrollment = b.invalidatedByBiometricEnrollment;
    strongBox = b.strongBox; unlockedDevice = b.unlockedDevice;
    authValiditySeconds = b.authValiditySeconds; authType = b.authType; maxUsageCount = b.maxUsageCount;
  }
  private static Date copy(Date d) { return d == null ? null : new Date(d.getTime()); }
  private static String[] copy(String[] a) { return a == null ? null : a.clone(); }

  public int getPurposes() { return purposes; }
  public Date getKeyValidityStart() { return copy(validityStart); }
  public Date getKeyValidityForOriginationEnd() { return copy(originationEnd); }
  public Date getKeyValidityForConsumptionEnd() { return copy(consumptionEnd); }
  public boolean isDigestsSpecified() { return digests != null; }
  public String[] getDigests() {
    if (digests == null) throw new IllegalStateException("Digests not specified");
    return digests.clone();
  }
  public String[] getEncryptionPaddings() { return encryptionPaddings == null ? new String[0] : encryptionPaddings.clone(); }
  public String[] getSignaturePaddings() { return signaturePaddings == null ? new String[0] : signaturePaddings.clone(); }
  public String[] getBlockModes() { return blockModes == null ? new String[0] : blockModes.clone(); }
  public boolean isRandomizedEncryptionRequired() { return randomizedEncryption; }
  public boolean isUserAuthenticationRequired() { return userAuthentication; }
  public boolean isUserConfirmationRequired() { return userConfirmation; }
  public int getUserAuthenticationValidityDurationSeconds() { return authValiditySeconds; }
  public int getUserAuthenticationType() { return authType; }
  public boolean isUserPresenceRequired() { return userPresence; }
  public boolean isUserAuthenticationValidWhileOnBody() { return validWhileOnBody; }
  public boolean isInvalidatedByBiometricEnrollment() { return invalidatedByBiometricEnrollment; }
  public boolean isStrongBoxBacked() { return strongBox; }
  public boolean isUnlockedDeviceRequired() { return unlockedDevice; }
  public int getMaxUsageCount() { return maxUsageCount; }

  public static final class Builder {
    private final int purposes;
    private Date validityStart, originationEnd, consumptionEnd;
    private String[] digests, encryptionPaddings, signaturePaddings, blockModes;
    private boolean randomizedEncryption = true, userAuthentication, userPresence, userConfirmation,
        validWhileOnBody, invalidatedByBiometricEnrollment = true, strongBox, unlockedDevice;
    private int authValiditySeconds, authType = KeyProperties.AUTH_BIOMETRIC_STRONG;
    private int maxUsageCount = KeyProperties.UNRESTRICTED_USAGE_COUNT;

    public Builder(int purposes) { this.purposes = purposes; }
    public Builder setKeyValidityStart(Date startDate) { validityStart = copy(startDate); return this; }
    public Builder setKeyValidityEnd(Date endDate) {
      setKeyValidityForOriginationEnd(endDate);
      return setKeyValidityForConsumptionEnd(endDate);
    }
    public Builder setKeyValidityForOriginationEnd(Date endDate) { originationEnd = copy(endDate); return this; }
    public Builder setKeyValidityForConsumptionEnd(Date endDate) { consumptionEnd = copy(endDate); return this; }
    public Builder setEncryptionPaddings(String... paddings) { encryptionPaddings = copy(paddings); return this; }
    public Builder setSignaturePaddings(String... paddings) { signaturePaddings = copy(paddings); return this; }
    public Builder setDigests(String... digests) { this.digests = copy(digests); return this; }
    public Builder setBlockModes(String... blockModes) { this.blockModes = copy(blockModes); return this; }
    public Builder setRandomizedEncryptionRequired(boolean required) { randomizedEncryption = required; return this; }
    public Builder setUserAuthenticationRequired(boolean required) { userAuthentication = required; return this; }
    public Builder setUserConfirmationRequired(boolean required) { userConfirmation = required; return this; }
    @Deprecated public Builder setUserAuthenticationValidityDurationSeconds(int seconds) {
      if (seconds < -1) throw new IllegalArgumentException("seconds must be -1 or larger");
      if (seconds == -1) return setUserAuthenticationParameters(0, KeyProperties.AUTH_BIOMETRIC_STRONG);
      return setUserAuthenticationParameters(seconds,
          KeyProperties.AUTH_DEVICE_CREDENTIAL | KeyProperties.AUTH_BIOMETRIC_STRONG);
    }
    public Builder setUserAuthenticationParameters(int timeout, int type) {
      if (timeout < 0) throw new IllegalArgumentException("timeout must be 0 or larger");
      authValiditySeconds = timeout; authType = type; return this;
    }
    public Builder setUserPresenceRequired(boolean required) { userPresence = required; return this; }
    public Builder setUserAuthenticationValidWhileOnBody(boolean remainsValid) { validWhileOnBody = remainsValid; return this; }
    public Builder setInvalidatedByBiometricEnrollment(boolean invalidate) { invalidatedByBiometricEnrollment = invalidate; return this; }
    public Builder setIsStrongBoxBacked(boolean backed) { strongBox = backed; return this; }
    public Builder setUnlockedDeviceRequired(boolean required) { unlockedDevice = required; return this; }
    public Builder setMaxUsageCount(int maxUsageCount) {
      if (maxUsageCount == KeyProperties.UNRESTRICTED_USAGE_COUNT || maxUsageCount > 0) {
        this.maxUsageCount = maxUsageCount; return this;
      }
      throw new IllegalArgumentException("maxUsageCount is not valid");
    }
    public KeyProtection build() { return new KeyProtection(this); }
  }
}
