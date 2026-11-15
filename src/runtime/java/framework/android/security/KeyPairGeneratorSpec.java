package android.security;

import android.content.Context;
import java.math.BigInteger;
import java.security.spec.AlgorithmParameterSpec;
import java.util.Date;
import javax.security.auth.x500.X500Principal;

/** The pre-API 23 parameters of a key pair generated in the Android Keystore (still in API 32). */
@Deprecated
public final class KeyPairGeneratorSpec implements AlgorithmParameterSpec {
  private final Context context;
  private final String alias, keyType;
  private final int keySize;
  private final AlgorithmParameterSpec spec;
  private final X500Principal subject;
  private final BigInteger serial;
  private final Date start, end;
  private final boolean encryptionRequired;

  public KeyPairGeneratorSpec(Context context, String keyStoreAlias, String keyType, int keySize,
      AlgorithmParameterSpec spec, X500Principal subjectDN, BigInteger serialNumber,
      Date startDate, Date endDate, int flags) {
    if (context == null) throw new IllegalArgumentException("context == null");
    if (keyStoreAlias == null || keyStoreAlias.isEmpty()) throw new IllegalArgumentException("keyStoreAlias must not be empty");
    if (subjectDN == null) throw new IllegalArgumentException("subjectDN == null");
    if (serialNumber == null) throw new IllegalArgumentException("serialNumber == null");
    if (startDate == null) throw new IllegalArgumentException("startDate == null");
    if (endDate == null) throw new IllegalArgumentException("endDate == null");
    if (endDate.before(startDate)) throw new IllegalArgumentException("endDate < startDate");
    this.context = context; alias = keyStoreAlias; this.keyType = keyType; this.keySize = keySize;
    this.spec = spec; subject = subjectDN; serial = serialNumber;
    start = new Date(startDate.getTime()); end = new Date(endDate.getTime());
    encryptionRequired = (flags & 1) != 0;
  }

  public Context getContext() { return context; }
  public String getKeystoreAlias() { return alias; }
  public String getKeyType() { return keyType; }
  public int getKeySize() { return keySize; }
  public AlgorithmParameterSpec getAlgorithmParameterSpec() { return spec; }
  public X500Principal getSubjectDN() { return subject; }
  public BigInteger getSerialNumber() { return serial; }
  public Date getStartDate() { return new Date(start.getTime()); }
  public Date getEndDate() { return new Date(end.getTime()); }
  public boolean isEncryptionRequired() { return encryptionRequired; }

  @Deprecated
  public static final class Builder {
    private final Context context;
    private String alias, keyType;
    private int keySize = -1;
    private AlgorithmParameterSpec spec;
    private X500Principal subject;
    private BigInteger serial;
    private Date start, end;
    private int flags;

    public Builder(Context context) {
      if (context == null) throw new NullPointerException("context == null");
      this.context = context;
    }
    public Builder setAlias(String alias) {
      if (alias == null) throw new NullPointerException("alias == null");
      this.alias = alias; return this;
    }
    public Builder setKeyType(String keyType) throws java.security.NoSuchAlgorithmException {
      if (keyType == null) throw new NullPointerException("keyType == null");
      if (!keyType.equalsIgnoreCase("RSA") && !keyType.equalsIgnoreCase("EC"))
        throw new java.security.NoSuchAlgorithmException("Unsupported key type: " + keyType);
      this.keyType = keyType; return this;
    }
    public Builder setKeySize(int keySize) {
      if (keySize < 0) throw new IllegalArgumentException("keySize < 0");
      this.keySize = keySize; return this;
    }
    public Builder setAlgorithmParameterSpec(AlgorithmParameterSpec spec) {
      if (spec == null) throw new NullPointerException("spec == null");
      this.spec = spec; return this;
    }
    public Builder setSubject(X500Principal subject) {
      if (subject == null) throw new NullPointerException("subject == null");
      this.subject = subject; return this;
    }
    public Builder setSerialNumber(BigInteger serialNumber) {
      if (serialNumber == null) throw new NullPointerException("serialNumber == null");
      this.serial = serialNumber; return this;
    }
    public Builder setStartDate(Date startDate) {
      if (startDate == null) throw new NullPointerException("startDate == null");
      this.start = startDate; return this;
    }
    public Builder setEndDate(Date endDate) {
      if (endDate == null) throw new NullPointerException("endDate == null");
      this.end = endDate; return this;
    }
    public Builder setEncryptionRequired() { flags |= 1; return this; }
    public KeyPairGeneratorSpec build() {
      return new KeyPairGeneratorSpec(context, alias, keyType, keySize, spec, subject, serial, start, end, flags);
    }
  }
}
