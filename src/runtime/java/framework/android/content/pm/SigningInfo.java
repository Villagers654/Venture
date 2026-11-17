package android.content.pm;
public final class SigningInfo {
  private final Signature[] signers, history;
  public SigningInfo(Signature[] s) {
    this(s, null);
  }
  /** history: the v3 proof-of-rotation certificates oldest first, or null when the key never rotated. */
  public SigningInfo(Signature[] s, Signature[] history) {
    signers = s.clone();
    this.history = history == null ? null : history.clone();
  }
  public boolean hasPastSigningCertificates() {
    return history != null && history.length > 1;
  }
  public boolean hasMultipleSigners() {
    return signers.length > 1;
  }
  public Signature[] getApkContentsSigners() {
    return signers.clone();
  }
  public Signature[] getSigningCertificateHistory() {
    // AOSP: null for multiple signers, the lineage after a rotation, else the current signer.
    if (hasMultipleSigners())
      return null;
    return hasPastSigningCertificates() ? history.clone() : signers.clone();
  }
}
