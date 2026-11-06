package com.venture.runtime;
import java.security.Security;
import java.util.ArrayList;
import java.util.List;
import java.util.Set;

/**
 * Game HTTPS (HttpsURLConnection, Unity's UnityWebRequest and its CustomSSLSocketFactory over
 * SSLContext "TLS") runs on the host JDK's JSSE instead of a Quest's Conscrypt. JDK 21 refuses
 * three things a Quest client still offers: TLS 1.0/1.1, static RSA key exchange (Conscrypt's
 * TLS_RSA_WITH_AES_{128,256}_{GCM,CBC} suites) and rsa_pkcs1_sha1 server signatures (in
 * BoringSSL's verify list). A server that only speaks those answers JDK's ClientHello with
 * handshake_failure or protocol_version, which a Quest never sees (Ghost Giant, issue 58).
 * This lifts exactly those entries from the JDK's disabled list and keeps the rest (SSLv3,
 * RC4, DES, 3DES, anon, NULL, static ECDH, weak keys), which Conscrypt refuses as well.
 * It must run before JSSE first reads the property, i.e. before any TLS use in the process.
 */
public final class TlsPolicy {
  private static final String PROPERTY = "jdk.tls.disabledAlgorithms";
  private static final Set<String> QUEST_CLIENT_ALLOWS =
      Set.of("tlsv1", "tlsv1.1", "tls_rsa_*", "rsa_pkcs1_sha1 usage handshakesignature");
  private static boolean installed;

  private TlsPolicy() {}

  public static synchronized void install() {
    if (installed) return;
    installed = true;
    String disabled = Security.getProperty(PROPERTY);
    if (disabled != null) Security.setProperty(PROPERTY, questClient(disabled));
  }

  public static String questClient(String disabled) {
    List<String> kept = new ArrayList<>();
    for (String entry : disabled.split(",")) {
      String trimmed = entry.trim().replaceAll("\\s+", " ");
      if (!trimmed.isEmpty() && !QUEST_CLIENT_ALLOWS.contains(trimmed.toLowerCase(java.util.Locale.ROOT)))
        kept.add(trimmed);
    }
    return String.join(", ", kept);
  }
}
