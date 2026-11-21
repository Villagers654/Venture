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
  /**
   * API 35's public form, which AOSP's SigningInfo also has (a 2D process runs
   * AOSP's, docs/design/layoutlib.md): past certificates oldest first.
   */
  public SigningInfo(int schemeVersion, java.util.Collection<Signature> signers,
                     java.util.Collection<java.security.PublicKey> publicKeys,
                     java.util.Collection<Signature> pastSigningCertificates) {
    this(signers.toArray(new Signature[0]),
         pastSigningCertificates == null || pastSigningCertificates.isEmpty() ? null
             : pastSigningCertificates.toArray(new Signature[0]));
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
