package com.venture.runtime;
import android.content.pm.Signature;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.file.*;
import java.security.GeneralSecurityException;
import java.util.HexFormat;

/**
 * Settings.Secure.ANDROID_ID as AOSP 8+ SettingsProvider generateSsaid computes it:
 * HMAC-SHA256 under a random per-user key over each length-prefixed signing
 * certificate, as 16 lowercase hex digits. Stable per user and signing key, so
 * it survives app reinstalls.
 */
public final class Ssaid {
  private Ssaid() {}
  /** Reads, or creates once, the per-user key in {@code userDir}. */
  public static byte[] userKey(Path userDir) throws IOException {
    Path file = userDir.resolve("settings_ssaid.key");
    if (Files.exists(file)) return HexFormat.of().parseHex(Files.readString(file).strip());
    byte[] key = new byte[32];
    new java.security.SecureRandom().nextBytes(key);
    Files.createDirectories(userDir);
    Path temp = Files.createTempFile(userDir, "ssaid-", ".tmp");
    try {
      Files.writeString(temp, HexFormat.of().formatHex(key));
      Files.move(temp, file, StandardCopyOption.ATOMIC_MOVE);
      return key;
    } catch (FileAlreadyExistsException raced) {
      return HexFormat.of().parseHex(Files.readString(file).strip());
    } finally {
      Files.deleteIfExists(temp);
    }
  }
  public static String derive(byte[] userKey, Signature[] signers) throws GeneralSecurityException {
    var mac = javax.crypto.Mac.getInstance("HmacSHA256");
    mac.init(new javax.crypto.spec.SecretKeySpec(userKey, mac.getAlgorithm()));
    for (var signer : signers) {
      byte[] certificate = signer.toByteArray();
      mac.update(ByteBuffer.allocate(4).putInt(certificate.length).array());
      mac.update(certificate);
    }
    return HexFormat.of().formatHex(mac.doFinal()).substring(0, 16);
  }
}
