import android.security.KeyPairGeneratorSpec;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import com.venture.runtime.AndroidKeyStore;
import java.math.BigInteger;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.KeyPairGenerator;
import java.security.KeyStore;
import java.security.PrivateKey;
import java.security.Signature;
import java.security.cert.X509Certificate;
import java.util.Arrays;
import java.util.Collections;
import java.util.Date;
import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.Mac;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;
import javax.crypto.spec.IvParameterSpec;
import javax.security.auth.x500.X500Principal;

/**
 * The "AndroidKeyStore" provider as Epic Online Services' keychain uses it (Into The Radius 2):
 * keys generated under an alias, used with the JDK's ciphers, and found again after a reload.
 */
public final class AndroidKeyStoreTest {
  private static int checks;
  private static void check(boolean ok, String message) {
    checks++;
    if (!ok) throw new AssertionError(message);
  }
  private static KeyStore open() throws Exception {
    KeyStore store = KeyStore.getInstance("AndroidKeyStore");
    store.load(null);
    return store;
  }

  public static void main(String[] args) throws Exception {
    Path dir = Files.createTempDirectory("venture-keystore");
    Path file = dir.resolve("AndroidKeyStore.p12");
    System.setProperty(AndroidKeyStore.FILE_PROPERTY, file.toString());
    try {
      run(file);
    } finally {
      try (var paths = Files.walk(dir)) {
        paths.sorted(Collections.reverseOrder()).forEach(p -> p.toFile().delete());
      }
    }
    System.out.println("AndroidKeyStoreTest: " + checks + " checks passed");
  }

  private static void run(Path file) throws Exception {
    AndroidKeyStore.install();
    AndroidKeyStore.install();
    KeyStore store = open();
    check(store.size() == 0 && !store.containsAlias("EOSKey"), "a new store is empty");

    // AES/GCM key under a mixed-case alias, as the EOS keychain makes it.
    KeyGenerator generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore");
    generator.init(new KeyGenParameterSpec.Builder("EOSKey",
        KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
        .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
        .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
        .setKeySize(256).build());
    SecretKey generated = generator.generateKey();
    check(generated.getAlgorithm().equals("AES"), "the generated key is an AES key");
    check(Files.isRegularFile(file), "generating a key persists the store");

    byte[] plain = "refresh-token-value".getBytes(StandardCharsets.UTF_8);
    Cipher gcm = Cipher.getInstance("AES/GCM/NoPadding");
    gcm.init(Cipher.ENCRYPT_MODE, generated);
    byte[] iv = gcm.getIV();
    byte[] sealed = gcm.doFinal(plain);
    check(iv != null && iv.length == 12 && !Arrays.equals(sealed, plain), "AES/GCM encrypts with a random IV");

    // A new KeyStore instance re-reads the file, as on the next launch.
    store = open();
    check(store.containsAlias("EOSKey") && !store.containsAlias("eoskey") && store.isKeyEntry("EOSKey")
        && Collections.list(store.aliases()).equals(java.util.List.of("EOSKey")), "the alias survives a reload, case-sensitive");
    SecretKey reloaded = (SecretKey) store.getKey("EOSKey", null);
    var entry = (KeyStore.SecretKeyEntry) store.getEntry("EOSKey", null);
    check(Arrays.equals(reloaded.getEncoded(), generated.getEncoded())
        && Arrays.equals(entry.getSecretKey().getEncoded(), generated.getEncoded()), "getKey and getEntry return the key");
    gcm = Cipher.getInstance("AES/GCM/NoPadding");
    gcm.init(Cipher.DECRYPT_MODE, reloaded, new GCMParameterSpec(128, iv));
    check(Arrays.equals(gcm.doFinal(sealed), plain), "data encrypted before the reload decrypts after it");
    check(store.getCreationDate("EOSKey") != null && store.getCertificate("EOSKey") == null, "secret keys have dates, no certificate");

    // Android's PKCS7Padding name for AES/CBC.
    generator.init(new KeyGenParameterSpec.Builder("cbc", KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
        .setBlockModes(KeyProperties.BLOCK_MODE_CBC).setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_PKCS7).build());
    SecretKey cbcKey = generator.generateKey();
    check(cbcKey.getEncoded().length == 16, "AES keys default to 128 bits");
    Cipher cbc = Cipher.getInstance("AES/CBC/PKCS7Padding");
    cbc.init(Cipher.ENCRYPT_MODE, cbcKey);
    byte[] cbcIv = cbc.getIV();
    byte[] cbcSealed = cbc.doFinal(plain);
    cbc = Cipher.getInstance("AES/CBC/PKCS7Padding");
    cbc.init(Cipher.DECRYPT_MODE, open().getKey("cbc", null), new IvParameterSpec(cbcIv));
    check(cbcSealed.length == 32 && Arrays.equals(cbc.doFinal(cbcSealed), plain), "AES/CBC/PKCS7Padding round-trips");

    // HMAC keys.
    KeyGenerator hmac = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_HMAC_SHA256, "AndroidKeyStore");
    hmac.init(new KeyGenParameterSpec.Builder("mac", KeyProperties.PURPOSE_SIGN).build());
    SecretKey macKey = hmac.generateKey();
    Mac mac = Mac.getInstance("HmacSHA256");
    mac.init(open().getKey("mac", null));
    byte[] tag = mac.doFinal(plain);
    mac.init(macKey);
    check(Arrays.equals(tag, mac.doFinal(plain)), "an HMAC key is found again after a reload");

    // Without a KeyGenParameterSpec, as on Android, initialization is refused.
    boolean refused = false;
    try { KeyGenerator.getInstance("AES", "AndroidKeyStore").init(128); } catch (UnsupportedOperationException e) { refused = true; }
    check(refused, "KeyGenerator.init(size) is refused");

    // RSA key pair with a self-signed certificate; RSA/ECB/PKCS1Padding and OAEP.
    KeyPairGenerator rsa = KeyPairGenerator.getInstance(KeyProperties.KEY_ALGORITHM_RSA, "AndroidKeyStore");
    rsa.initialize(new KeyGenParameterSpec.Builder("rsa", KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
        .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_RSA_PKCS1, KeyProperties.ENCRYPTION_PADDING_RSA_OAEP)
        .setKeySize(2048).build());
    var pair = rsa.generateKeyPair();
    store = open();
    var cert = (X509Certificate) store.getCertificate("rsa");
    cert.verify(cert.getPublicKey());
    check(cert.getSubjectX500Principal().equals(new X500Principal("CN=fake")) && cert.getSerialNumber().equals(BigInteger.ONE)
        && cert.getPublicKey().equals(pair.getPublic()) && store.getCertificateChain("rsa").length == 1
        && "rsa".equals(store.getCertificateAlias(cert)), "an RSA key pair carries a self-signed certificate");
    for (String transformation : new String[] {"RSA/ECB/PKCS1Padding", "RSA/ECB/OAEPWithSHA-256AndMGF1Padding"}) {
      Cipher c = Cipher.getInstance(transformation);
      c.init(Cipher.ENCRYPT_MODE, store.getCertificate("rsa").getPublicKey());
      byte[] wrapped = c.doFinal(plain);
      c = Cipher.getInstance(transformation);
      c.init(Cipher.DECRYPT_MODE, (PrivateKey) store.getKey("rsa", null));
      check(Arrays.equals(c.doFinal(wrapped), plain), transformation + " round-trips with the stored key pair");
    }

    // EC key pair from the legacy KeyPairGeneratorSpec, signing with SHA256withECDSA.
    KeyPairGenerator ec = KeyPairGenerator.getInstance(KeyProperties.KEY_ALGORITHM_EC, "AndroidKeyStore");
    ec.initialize(new KeyPairGeneratorSpec.Builder(new android.content.Context()).setAlias("ec")
        .setSubject(new X500Principal("CN=ec")).setSerialNumber(BigInteger.TEN)
        .setStartDate(new Date(0)).setEndDate(new Date(4102444800000L)).build());
    ec.generateKeyPair();
    store = open();
    var entryEc = (KeyStore.PrivateKeyEntry) store.getEntry("ec", null);
    Signature signer = Signature.getInstance("SHA256withECDSA");
    signer.initSign(entryEc.getPrivateKey());
    signer.update(plain);
    byte[] signature = signer.sign();
    signer.initVerify(entryEc.getCertificate());
    signer.update(plain);
    check(signer.verify(signature) && ((X509Certificate) entryEc.getCertificate()).getNotAfter().getTime() == 4102444800000L,
        "an EC key pair signs and its certificate keeps the requested validity");

    // deleteEntry persists too.
    store.deleteEntry("cbc");
    store.deleteEntry("missing");
    store = open();
    check(!store.containsAlias("cbc") && store.containsAlias("EOSKey") && store.size() == 4, "deleteEntry persists");

    // An unreadable file is set aside rather than failing the game.
    Files.write(file, new byte[] {1, 2, 3});
    store = open();
    check(store.size() == 0 && Files.exists(file.resolveSibling(file.getFileName() + ".unreadable")),
        "an unreadable store loads empty and is kept aside");
  }
}
