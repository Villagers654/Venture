package com.venture.runtime;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.file.Path;
import java.security.Key;
import java.security.KeyStore;
import java.security.KeyStoreSpi;
import java.security.Provider;
import java.security.Security;
import java.security.cert.Certificate;
import java.security.cert.CertificateException;
import java.security.cert.X509Certificate;
import java.util.Collections;
import java.util.Date;
import java.util.Enumeration;
import java.util.LinkedHashMap;
import java.util.Map;
import javax.security.auth.x500.X500Principal;

/**
 * Android's read-only "AndroidCAStore" KeyStore of trusted system roots, here the
 * runtime JDK's own CA certificates. Unity's TLS reads its roots from it on a
 * worker thread and treats the lookup failing as a fatal crash (Alcove).
 * Aliases follow AOSP TrustedCertificateStore: "system:" + subject hash + ".0".
 */
public final class AndroidCAStore extends KeyStoreSpi {
  private static boolean installed;
  private final Map<String, X509Certificate> roots = new LinkedHashMap<>();

  public static synchronized void install() {
    if (installed) return;
    installed = true;
    Provider provider = new Provider("AndroidCAStore", "1.0", "Android system CA certificates") {};
    provider.put("KeyStore.AndroidCAStore", AndroidCAStore.class.getName());
    Security.addProvider(provider);
  }

  @Override
  public void engineLoad(InputStream stream, char[] password) throws IOException, CertificateException {
    roots.clear();
    KeyStore jdk;
    try {
      jdk = KeyStore.getInstance(Path.of(System.getProperty("java.home"), "lib", "security", "cacerts").toFile(),
          (char[]) null);
      for (String alias : Collections.list(jdk.aliases()))
        if (jdk.getCertificate(alias) instanceof X509Certificate cert) {
          String name = "system:" + subjectHash(cert.getSubjectX500Principal());
          int n = 0;
          while (roots.containsKey(name + "." + n)) n++;
          roots.put(name + "." + n, cert);
        }
    } catch (java.security.GeneralSecurityException e) {
      throw new IOException("System CA certificates unavailable", e);
    }
  }
  // OpenSSL X509_NAME_hash_old is MD5-based; any stable 8-hex-digit name works for callers.
  private static String subjectHash(X500Principal subject) {
    return String.format("%08x", subject.hashCode());
  }

  @Override public Enumeration<String> engineAliases() { return Collections.enumeration(roots.keySet()); }
  @Override public boolean engineContainsAlias(String alias) { return roots.containsKey(alias); }
  @Override public int engineSize() { return roots.size(); }
  @Override public Certificate engineGetCertificate(String alias) { return roots.get(alias); }
  @Override public Certificate[] engineGetCertificateChain(String alias) { return null; }
  @Override public Key engineGetKey(String alias, char[] password) { return null; }
  @Override public Date engineGetCreationDate(String alias) {
    X509Certificate cert = roots.get(alias);
    return cert == null ? null : cert.getNotBefore();
  }
  @Override public boolean engineIsKeyEntry(String alias) { return false; }
  @Override public boolean engineIsCertificateEntry(String alias) { return roots.containsKey(alias); }
  @Override public String engineGetCertificateAlias(Certificate cert) {
    for (var e : roots.entrySet())
      if (e.getValue().equals(cert)) return e.getKey();
    return null;
  }
  @Override public void engineSetKeyEntry(String alias, Key key, char[] password, Certificate[] chain) {
    throw new UnsupportedOperationException();
  }
  @Override public void engineSetKeyEntry(String alias, byte[] key, Certificate[] chain) {
    throw new UnsupportedOperationException();
  }
  @Override public void engineSetCertificateEntry(String alias, Certificate cert) {
    throw new UnsupportedOperationException();
  }
  @Override public void engineDeleteEntry(String alias) { throw new UnsupportedOperationException(); }
  @Override public void engineStore(OutputStream stream, char[] password) { throw new UnsupportedOperationException(); }
}
