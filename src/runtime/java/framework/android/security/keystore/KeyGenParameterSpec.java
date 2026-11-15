package android.security.keystore;

import java.math.BigInteger;
import java.security.spec.AlgorithmParameterSpec;
import java.util.Date;
import javax.security.auth.x500.X500Principal;

/**
 * Parameters of a key generated in the Android Keystore (API 32). The Venture
 * "AndroidKeyStore" provider (com.venture.runtime.AndroidKeyStore) consumes it.
 */
public final class KeyGenParameterSpec implements AlgorithmParameterSpec {
  private static final X500Principal DEFAULT_SUBJECT = new X500Principal("CN=fake");
  private static final BigInteger DEFAULT_SERIAL = BigInteger.ONE;
  private static final Date DEFAULT_NOT_BEFORE = new Date(0L); // 1970-01-01
  private static final Date DEFAULT_NOT_AFTER = new Date(2461449600000L); // 2048-01-01

  private final String alias;
  private final int keySize, purposes;
  private final AlgorithmParameterSpec spec;
  private final X500Principal subject;
  private final BigInteger serial;
  private final Date notBefore, notAfter, validityStart, originationEnd, consumptionEnd;
  private final String[] digests, encryptionPaddings, signaturePaddings, blockModes;
  private final boolean randomizedEncryption, userAuthentication, userPresence, userConfirmation,
      devicePropertiesAttestation, uniqueId, validWhileOnBody, invalidatedByBiometricEnrollment,
      strongBox, unlockedDevice;
  private final int authValiditySeconds, authType, maxUsageCount;
  private final byte[] attestationChallenge;

  private KeyGenParameterSpec(Builder b) {
    alias = b.alias; keySize = b.keySize; purposes = b.purposes; spec = b.spec;
    subject = b.subject == null ? DEFAULT_SUBJECT : b.subject;
    serial = b.serial == null ? DEFAULT_SERIAL : b.serial;
    notBefore = copy(b.notBefore == null ? DEFAULT_NOT_BEFORE : b.notBefore);
    notAfter = copy(b.notAfter == null ? DEFAULT_NOT_AFTER : b.notAfter);
    validityStart = copy(b.validityStart); originationEnd = copy(b.originationEnd);
    consumptionEnd = copy(b.consumptionEnd);
    digests = copy(b.digests); encryptionPaddings = copy(b.encryptionPaddings);
    signaturePaddings = copy(b.signaturePaddings); blockModes = copy(b.blockModes);
    randomizedEncryption = b.randomizedEncryption; userAuthentication = b.userAuthentication;
    userPresence = b.userPresence; userConfirmation = b.userConfirmation;
    devicePropertiesAttestation = b.devicePropertiesAttestation; uniqueId = b.uniqueId;
    validWhileOnBody = b.validWhileOnBody;
    invalidatedByBiometricEnrollment = b.invalidatedByBiometricEnrollment;
    strongBox = b.strongBox; unlockedDevice = b.unlockedDevice;
    authValiditySeconds = b.authValiditySeconds; authType = b.authType;
    maxUsageCount = b.maxUsageCount;
    attestationChallenge = b.attestationChallenge == null ? null : b.attestationChallenge.clone();
  }
  private static Date copy(Date d) { return d == null ? null : new Date(d.getTime()); }
  private static String[] copy(String[] a) { return a == null ? null : a.clone(); }

  public String getKeystoreAlias() { return alias; }
  public int getPurposes() { return purposes; }
  public int getKeySize() { return keySize; }
  public AlgorithmParameterSpec getAlgorithmParameterSpec() { return spec; }
  public X500Principal getCertificateSubject() { return subject; }
  public BigInteger getCertificateSerialNumber() { return serial; }
  public Date getCertificateNotBefore() { return copy(notBefore); }
  public Date getCertificateNotAfter() { return copy(notAfter); }
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
  public byte[] getAttestationChallenge() { return attestationChallenge == null ? null : attestationChallenge.clone(); }
  public boolean isDevicePropertiesAttestationIncluded() { return devicePropertiesAttestation; }
  public boolean isUniqueIdIncluded() { return uniqueId; }
  public boolean isUserAuthenticationValidWhileOnBody() { return validWhileOnBody; }
  public boolean isInvalidatedByBiometricEnrollment() { return invalidatedByBiometricEnrollment; }
  public boolean isStrongBoxBacked() { return strongBox; }
  public boolean isUnlockedDeviceRequired() { return unlockedDevice; }
  public int getMaxUsageCount() { return maxUsageCount; }

  public static final class Builder {
    private final String alias;
    private int purposes;
    private int keySize = -1;
    private AlgorithmParameterSpec spec;
    private X500Principal subject;
    private BigInteger serial;
    private Date notBefore, notAfter, validityStart, originationEnd, consumptionEnd;
    private String[] digests, encryptionPaddings, signaturePaddings, blockModes;
    private boolean randomizedEncryption = true, userAuthentication, userPresence, userConfirmation,
        devicePropertiesAttestation, uniqueId, validWhileOnBody, invalidatedByBiometricEnrollment = true,
        strongBox, unlockedDevice;
    private int authValiditySeconds, authType = KeyProperties.AUTH_BIOMETRIC_STRONG;
    private int maxUsageCount = KeyProperties.UNRESTRICTED_USAGE_COUNT;
    private byte[] attestationChallenge;

    public Builder(String keystoreAlias, int purposes) {
      if (keystoreAlias == null) throw new NullPointerException("keystoreAlias == null");
      if (keystoreAlias.isEmpty()) throw new IllegalArgumentException("keystoreAlias must not be empty");
      this.alias = keystoreAlias;
      this.purposes = purposes;
    }
    public Builder(KeyGenParameterSpec s) {
      this(s.alias, s.purposes);
      keySize = s.keySize; spec = s.spec; subject = s.subject; serial = s.serial;
      notBefore = s.notBefore; notAfter = s.notAfter; validityStart = s.validityStart;
      originationEnd = s.originationEnd; consumptionEnd = s.consumptionEnd;
      digests = s.digests; encryptionPaddings = s.encryptionPaddings;
      signaturePaddings = s.signaturePaddings; blockModes = s.blockModes;
      randomizedEncryption = s.randomizedEncryption; userAuthentication = s.userAuthentication;
      userPresence = s.userPresence; userConfirmation = s.userConfirmation;
      devicePropertiesAttestation = s.devicePropertiesAttestation; uniqueId = s.uniqueId;
      validWhileOnBody = s.validWhileOnBody;
      invalidatedByBiometricEnrollment = s.invalidatedByBiometricEnrollment;
      strongBox = s.strongBox; unlockedDevice = s.unlockedDevice;
      authValiditySeconds = s.authValiditySeconds; authType = s.authType;
      maxUsageCount = s.maxUsageCount; attestationChallenge = s.attestationChallenge;
    }
    public Builder setKeySize(int keySize) {
      if (keySize < 0) throw new IllegalArgumentException("keySize < 0");
      this.keySize = keySize; return this;
    }
    public Builder setAlgorithmParameterSpec(AlgorithmParameterSpec spec) {
      if (spec == null) throw new NullPointerException("spec == null");
      this.spec = spec; return this;
    }
    public Builder setCertificateSubject(X500Principal subject) {
      if (subject == null) throw new NullPointerException("subject == null");
      this.subject = subject; return this;
    }
    public Builder setCertificateSerialNumber(BigInteger serialNumber) {
      if (serialNumber == null) throw new NullPointerException("serialNumber == null");
      this.serial = serialNumber; return this;
    }
    public Builder setCertificateNotBefore(Date date) {
      if (date == null) throw new NullPointerException("date == null");
      this.notBefore = copy(date); return this;
    }
    public Builder setCertificateNotAfter(Date date) {
      if (date == null) throw new NullPointerException("date == null");
      this.notAfter = copy(date); return this;
    }
    public Builder setKeyValidityStart(Date startDate) { validityStart = copy(startDate); return this; }
    public Builder setKeyValidityEnd(Date endDate) {
      setKeyValidityForOriginationEnd(endDate);
      return setKeyValidityForConsumptionEnd(endDate);
    }
    public Builder setKeyValidityForOriginationEnd(Date endDate) { originationEnd = copy(endDate); return this; }
    public Builder setKeyValidityForConsumptionEnd(Date endDate) { consumptionEnd = copy(endDate); return this; }
    public Builder setDigests(String... digests) { this.digests = copy(digests); return this; }
    public Builder setEncryptionPaddings(String... paddings) { encryptionPaddings = copy(paddings); return this; }
    public Builder setSignaturePaddings(String... paddings) { signaturePaddings = copy(paddings); return this; }
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
    public Builder setAttestationChallenge(byte[] challenge) { attestationChallenge = challenge; return this; }
    public Builder setDevicePropertiesAttestationIncluded(boolean included) { devicePropertiesAttestation = included; return this; }
    public Builder setUniqueIdIncluded(boolean included) { uniqueId = included; return this; }
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
    public KeyGenParameterSpec build() { return new KeyGenParameterSpec(this); }
  }
}
