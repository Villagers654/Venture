package android.content.pm;
public final class SigningInfo {
  private final Signature[] signers;
  public SigningInfo(Signature[] s) {
    signers = s.clone();
  }
  public boolean hasMultipleSigners() {
    return signers.length > 1;
  }
  public Signature[] getApkContentsSigners() {
    return signers.clone();
  }
  public Signature[] getSigningCertificateHistory() {
    if (hasMultipleSigners())
      return null;
    throw new UnsupportedOperationException("APK proof-of-rotation not implemented");
  }
}
