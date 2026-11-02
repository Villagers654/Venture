package android.net.http;

import java.security.cert.CertificateException;
import java.security.cert.X509Certificate;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import javax.net.ssl.X509TrustManager;

/**
 * Android's hostname-aware trust check. OkHttp's certificate chain cleaner uses it on
 * Android 10+ (Roblox): the result is the verified chain from the leaf up to and
 * including its trust anchor, which certificate pinning then matches against.
 */
public class X509TrustManagerExtensions {
  private final X509TrustManager trustManager;

  public X509TrustManagerExtensions(X509TrustManager tm) throws IllegalArgumentException {
    if (tm == null) throw new IllegalArgumentException("tm is null");
    trustManager = tm;
  }

  public List<X509Certificate> checkServerTrusted(X509Certificate[] chain, String authType, String host)
      throws CertificateException {
    if (chain == null || chain.length == 0) throw new IllegalArgumentException("chain is empty");
    trustManager.checkServerTrusted(chain, authType);
    List<X509Certificate> candidates = new ArrayList<>(Arrays.asList(chain));
    candidates.addAll(Arrays.asList(trustManager.getAcceptedIssuers()));
    List<X509Certificate> path = new ArrayList<>();
    X509Certificate current = chain[0];
    path.add(current);
    while (!issuedBy(current, current) && path.size() <= candidates.size()) {
      X509Certificate issuer = null;
      for (X509Certificate candidate : candidates)
        if (!path.contains(candidate) && issuedBy(current, candidate)) { issuer = candidate; break; }
      if (issuer == null) break;
      path.add(issuer);
      current = issuer;
    }
    return path;
  }

  private static boolean issuedBy(X509Certificate child, X509Certificate issuer) {
    if (!child.getIssuerX500Principal().equals(issuer.getSubjectX500Principal())) return false;
    try {
      child.verify(issuer.getPublicKey());
      return true;
    } catch (Exception e) {
      return false;
    }
  }

  public boolean isUserAddedCertificate(X509Certificate cert) { return false; }

  public boolean isSameTrustConfiguration(String hostname1, String hostname2) { return true; }
}
