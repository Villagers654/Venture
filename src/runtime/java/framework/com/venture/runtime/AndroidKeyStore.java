package com.venture.runtime;

import android.security.KeyPairGeneratorSpec;
import android.security.keystore.KeyGenParameterSpec;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.math.BigInteger;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.security.AlgorithmParameters;
import java.security.GeneralSecurityException;
import java.security.InvalidAlgorithmParameterException;
import java.security.InvalidKeyException;
import java.security.InvalidParameterException;
import java.security.Key;
import java.security.KeyPair;
import java.security.KeyPairGenerator;
import java.security.KeyPairGeneratorSpi;
import java.security.KeyStore;
import java.security.KeyStoreException;
import java.security.KeyStoreSpi;
import java.security.NoSuchAlgorithmException;
import java.security.PrivateKey;
import java.security.Provider;
import java.security.ProviderException;
import java.security.SecureRandom;
import java.security.Security;
import java.security.Signature;
import java.security.cert.Certificate;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;
import java.security.spec.AlgorithmParameterSpec;
import java.security.spec.ECGenParameterSpec;
import java.security.spec.RSAKeyGenParameterSpec;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Date;
import java.util.Enumeration;
import java.util.HexFormat;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;
import java.util.TimeZone;
import javax.crypto.BadPaddingException;
import javax.crypto.Cipher;
import javax.crypto.CipherSpi;
import javax.crypto.IllegalBlockSizeException;
import javax.crypto.KeyGeneratorSpi;
import javax.crypto.NoSuchPaddingException;
import javax.crypto.SecretKey;
import javax.crypto.ShortBufferException;
import javax.crypto.spec.SecretKeySpec;
import javax.security.auth.x500.X500Principal;

/**
 * Android's "AndroidKeyStore" JCA provider: KeyStore, KeyGenerator (AES, HMAC) and
 * KeyPairGenerator (RSA, EC) driven by KeyGenParameterSpec. Keys are ordinary JCA keys
 * that the JDK's own Cipher, Mac and Signature implementations use, persisted per app in
 * a PKCS12 file in its private data directory, so data a game encrypted stays readable on
 * the next launch. Epic Online Services keeps its keychain under such a key (Into The
 * Radius 2). The provider also maps Android's PKCS7Padding AES names to the JDK's PKCS5.
 */
public final class AndroidKeyStore extends KeyStoreSpi {
  public static final String NAME = "AndroidKeyStore";
  /** The PKCS12 file; tests point it at a temporary path when no application is configured. */
  public static final String FILE_PROPERTY = "venture.androidKeyStore.file";
  private static final char[] PASSWORD = "venture-android-keystore".toCharArray();
  private static final Object lock = new Object();
  private static final Map<String, KeyStore.Entry> entries = new LinkedHashMap<>();
  private static final Map<String, Date> dates = new LinkedHashMap<>();
  private static boolean loaded, installed;

  public static synchronized void install() {
    if (installed) return;
    installed = true;
    Provider provider = new Provider(NAME, "1.0", "Android Keystore") {};
    provider.put("KeyStore." + NAME, AndroidKeyStore.class.getName());
    provider.put("KeyGenerator.AES", Aes.class.getName());
    provider.put("KeyGenerator.HmacSHA1", HmacSha1.class.getName());
    provider.put("KeyGenerator.HmacSHA224", HmacSha224.class.getName());
    provider.put("KeyGenerator.HmacSHA256", HmacSha256.class.getName());
    provider.put("KeyGenerator.HmacSHA384", HmacSha384.class.getName());
    provider.put("KeyGenerator.HmacSHA512", HmacSha512.class.getName());
    provider.put("KeyPairGenerator.RSA", Rsa.class.getName());
    provider.put("KeyPairGenerator.EC", Ec.class.getName());
    provider.put("Cipher.AES/CBC/PKCS7Padding", AesCbcPkcs7.class.getName());
    provider.put("Cipher.AES/ECB/PKCS7Padding", AesEcbPkcs7.class.getName());
    Security.addProvider(provider);
  }

  // ---- Persistence ----

  private static File file() {
    String path = System.getProperty(FILE_PROPERTY);
    if (path != null && !path.isEmpty()) return new File(path);
    return new File(RuntimeEnvironment.get().data, ".venture/AndroidKeyStore.p12");
  }
  // PKCS12 lowercases aliases; Android Keystore aliases are case-sensitive.
  private static String encode(String alias) {
    return "k" + HexFormat.of().formatHex(alias.getBytes(StandardCharsets.UTF_8));
  }
  private static String decode(String stored) {
    return new String(HexFormat.of().parseHex(stored.substring(1)), StandardCharsets.UTF_8);
  }
  private static void read() throws IOException {
    entries.clear();
    dates.clear();
    loaded = true;
    File f = file();
    if (!f.isFile()) return;
    byte[] bytes = Files.readAllBytes(f.toPath());
    try {
      KeyStore p12 = KeyStore.getInstance("PKCS12");
      p12.load(new ByteArrayInputStream(bytes), PASSWORD);
      for (String stored : Collections.list(p12.aliases())) {
        KeyStore.Entry entry = p12.getEntry(stored,
            p12.isCertificateEntry(stored) ? null : new KeyStore.PasswordProtection(PASSWORD));
        String alias = decode(stored);
        entries.put(alias, entry);
        Date date = p12.getCreationDate(stored);
        dates.put(alias, date == null ? new Date() : date);
      }
    } catch (IOException | GeneralSecurityException | RuntimeException e) {
      // An unreadable store holds no usable key; keep it aside rather than stop the game.
      entries.clear();
      dates.clear();
      File aside = new File(f.getPath() + ".unreadable");
      Files.move(f.toPath(), aside.toPath(), StandardCopyOption.REPLACE_EXISTING);
    }
  }
  private static void ensureLoaded() {
    if (loaded) return;
    try { read(); } catch (IOException e) { throw new ProviderException("Android Keystore is unreadable", e); }
  }
  private static void write() throws IOException, GeneralSecurityException {
    KeyStore p12 = KeyStore.getInstance("PKCS12");
    p12.load(null, null);
    for (var e : entries.entrySet())
      p12.setEntry(encode(e.getKey()), e.getValue(),
          e.getValue() instanceof KeyStore.TrustedCertificateEntry ? null : new KeyStore.PasswordProtection(PASSWORD));
    var bytes = new ByteArrayOutputStream();
    p12.store(bytes, PASSWORD);
    File f = file();
    File dir = f.getAbsoluteFile().getParentFile();
    if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("Cannot create " + dir);
    File tmp = new File(dir, f.getName() + ".tmp");
    Files.write(tmp.toPath(), bytes.toByteArray());
    Files.move(tmp.toPath(), f.toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
  }
  private static void put(String alias, KeyStore.Entry entry) {
    synchronized (lock) {
      ensureLoaded();
      KeyStore.Entry previous = entries.put(alias, entry);
      Date previousDate = dates.put(alias, new Date());
      try {
        write();
      } catch (IOException | GeneralSecurityException e) {
        if (previous == null) { entries.remove(alias); dates.remove(alias); }
        else { entries.put(alias, previous); dates.put(alias, previousDate); }
        throw new ProviderException("Failed to store key " + alias + " in the Android Keystore", e);
      }
    }
  }
  private static void putChecked(String alias, KeyStore.Entry entry) throws KeyStoreException {
    try { put(alias, entry); } catch (ProviderException e) { throw new KeyStoreException(e.getMessage(), e.getCause()); }
  }
  private static KeyStore.Entry entry(String alias) {
    synchronized (lock) {
      ensureLoaded();
      return entries.get(alias);
    }
  }

  // ---- KeyStoreSpi ----

  @Override public void engineLoad(InputStream stream, char[] password) throws IOException {
    if (stream != null) throw new IllegalArgumentException("InputStream not supported");
    if (password != null) throw new IllegalArgumentException("password not supported");
    synchronized (lock) { read(); }
  }
  @Override public void engineLoad(KeyStore.LoadStoreParameter param) throws IOException {
    synchronized (lock) { read(); }
  }
  @Override public void engineStore(OutputStream stream, char[] password) {
    throw new UnsupportedOperationException("Can not serialize AndroidKeyStore to OutputStream");
  }
  @Override public Enumeration<String> engineAliases() {
    synchronized (lock) { ensureLoaded(); return Collections.enumeration(new ArrayList<>(entries.keySet())); }
  }
  @Override public boolean engineContainsAlias(String alias) { return alias != null && entry(alias) != null; }
  @Override public int engineSize() { synchronized (lock) { ensureLoaded(); return entries.size(); } }
  @Override public Key engineGetKey(String alias, char[] password) {
    KeyStore.Entry e = entry(alias);
    if (e instanceof KeyStore.PrivateKeyEntry p) return p.getPrivateKey();
    if (e instanceof KeyStore.SecretKeyEntry s) return s.getSecretKey();
    return null;
  }
  @Override public Certificate[] engineGetCertificateChain(String alias) {
    return entry(alias) instanceof KeyStore.PrivateKeyEntry p ? p.getCertificateChain() : null;
  }
  @Override public Certificate engineGetCertificate(String alias) {
    KeyStore.Entry e = entry(alias);
    if (e instanceof KeyStore.PrivateKeyEntry p) return p.getCertificate();
    if (e instanceof KeyStore.TrustedCertificateEntry t) return t.getTrustedCertificate();
    return null;
  }
  @Override public Date engineGetCreationDate(String alias) {
    synchronized (lock) {
      ensureLoaded();
      Date d = dates.get(alias);
      return d == null ? null : new Date(d.getTime());
    }
  }
  @Override public boolean engineIsKeyEntry(String alias) {
    KeyStore.Entry e = entry(alias);
    return e instanceof KeyStore.PrivateKeyEntry || e instanceof KeyStore.SecretKeyEntry;
  }
  @Override public boolean engineIsCertificateEntry(String alias) {
    return entry(alias) instanceof KeyStore.TrustedCertificateEntry;
  }
  @Override public String engineGetCertificateAlias(Certificate cert) {
    if (cert == null) return null;
    synchronized (lock) {
      ensureLoaded();
      for (var e : entries.entrySet()) {
        Certificate c = e.getValue() instanceof KeyStore.PrivateKeyEntry p ? p.getCertificate()
            : e.getValue() instanceof KeyStore.TrustedCertificateEntry t ? t.getTrustedCertificate() : null;
        if (cert.equals(c)) return e.getKey();
      }
      return null;
    }
  }
  @Override public void engineSetKeyEntry(String alias, Key key, char[] password, Certificate[] chain)
      throws KeyStoreException {
    if (alias == null) throw new NullPointerException("alias == null");
    if (key instanceof PrivateKey p) {
      if (chain == null || chain.length == 0) throw new KeyStoreException("Private key requires a certificate chain");
      putChecked(alias, new KeyStore.PrivateKeyEntry(p, chain));
    } else if (key instanceof SecretKey s) {
      putChecked(alias, new KeyStore.SecretKeyEntry(s));
    } else {
      throw new KeyStoreException("Unsupported key type: " + (key == null ? "null" : key.getClass().getName()));
    }
  }
  @Override public void engineSetKeyEntry(String alias, byte[] key, Certificate[] chain) throws KeyStoreException {
    throw new KeyStoreException("Operation not supported because key encoding is unknown");
  }
  @Override public void engineSetCertificateEntry(String alias, Certificate cert) throws KeyStoreException {
    if (alias == null) throw new NullPointerException("alias == null");
    if (cert == null) throw new NullPointerException("cert == null");
    putChecked(alias, new KeyStore.TrustedCertificateEntry(cert));
  }
  @Override public void engineDeleteEntry(String alias) throws KeyStoreException {
    synchronized (lock) {
      ensureLoaded();
      KeyStore.Entry previous = entries.remove(alias);
      if (previous == null) return;
      Date previousDate = dates.remove(alias);
      try {
        write();
      } catch (IOException | GeneralSecurityException e) {
        entries.put(alias, previous);
        dates.put(alias, previousDate);
        throw new KeyStoreException("Failed to delete " + alias, e);
      }
    }
  }
  @Override public KeyStore.Entry engineGetEntry(String alias, KeyStore.ProtectionParameter protection) {
    return alias == null ? null : entry(alias);
  }
  @Override public void engineSetEntry(String alias, KeyStore.Entry entry, KeyStore.ProtectionParameter protection)
      throws KeyStoreException {
    if (alias == null) throw new NullPointerException("alias == null");
    if (entry instanceof KeyStore.PrivateKeyEntry || entry instanceof KeyStore.SecretKeyEntry
        || entry instanceof KeyStore.TrustedCertificateEntry)
      putChecked(alias, entry);
    else
      throw new KeyStoreException("Entry must be a PrivateKeyEntry, SecretKeyEntry or TrustedCertificateEntry");
  }

  // ---- KeyGenerator ----

  public abstract static class SecretKeys extends KeyGeneratorSpi {
    private final String algorithm;
    private final int defaultBits;
    private KeyGenParameterSpec spec;
    private int bits;
    private SecureRandom random;
    protected SecretKeys(String algorithm, int defaultBits) { this.algorithm = algorithm; this.defaultBits = defaultBits; }
    @Override protected void engineInit(SecureRandom random) {
      throw new UnsupportedOperationException("Cannot initialize without a " + KeyGenParameterSpec.class.getName() + " parameter");
    }
    @Override protected void engineInit(int keySize, SecureRandom random) { engineInit(random); }
    @Override protected void engineInit(AlgorithmParameterSpec params, SecureRandom random)
        throws InvalidAlgorithmParameterException {
      if (!(params instanceof KeyGenParameterSpec s))
        throw new InvalidAlgorithmParameterException("Cannot initialize without a " + KeyGenParameterSpec.class.getName() + " parameter");
      int size = s.getKeySize() == -1 ? defaultBits : s.getKeySize();
      boolean valid = algorithm.equals("AES") ? size == 128 || size == 192 || size == 256
          : size >= 64 && size <= 512 && size % 8 == 0;
      if (!valid) throw new InvalidAlgorithmParameterException("Unsupported key size: " + size);
      spec = s;
      bits = size;
      this.random = random == null ? new SecureRandom() : random;
    }
    @Override protected SecretKey engineGenerateKey() {
      if (spec == null) throw new IllegalStateException("Not initialized");
      byte[] material = new byte[bits / 8];
      random.nextBytes(material);
      SecretKey key = new SecretKeySpec(material, algorithm);
      put(spec.getKeystoreAlias(), new KeyStore.SecretKeyEntry(key));
      return key;
    }
  }
  public static final class Aes extends SecretKeys { public Aes() { super("AES", 128); } }
  public static final class HmacSha1 extends SecretKeys { public HmacSha1() { super("HmacSHA1", 160); } }
  public static final class HmacSha224 extends SecretKeys { public HmacSha224() { super("HmacSHA224", 224); } }
  public static final class HmacSha256 extends SecretKeys { public HmacSha256() { super("HmacSHA256", 256); } }
  public static final class HmacSha384 extends SecretKeys { public HmacSha384() { super("HmacSHA384", 384); } }
  public static final class HmacSha512 extends SecretKeys { public HmacSha512() { super("HmacSHA512", 512); } }

  // ---- KeyPairGenerator ----

  public abstract static class KeyPairs extends KeyPairGeneratorSpi {
    private final String algorithm;
    private String alias;
    private int bits;
    private AlgorithmParameterSpec params;
    private X500Principal subject;
    private BigInteger serial;
    private Date notBefore, notAfter;
    private SecureRandom random;
    protected KeyPairs(String algorithm) { this.algorithm = algorithm; }
    @Override public void initialize(int keySize, SecureRandom random) {
      throw new IllegalArgumentException(KeyGenParameterSpec.class.getName() + " or "
          + KeyPairGeneratorSpec.class.getName() + " required to initialize this KeyPairGenerator");
    }
    @Override public void initialize(AlgorithmParameterSpec spec, SecureRandom random)
        throws InvalidAlgorithmParameterException {
      if (spec instanceof KeyGenParameterSpec s) {
        alias = s.getKeystoreAlias(); bits = s.getKeySize(); params = s.getAlgorithmParameterSpec();
        subject = s.getCertificateSubject(); serial = s.getCertificateSerialNumber();
        notBefore = s.getCertificateNotBefore(); notAfter = s.getCertificateNotAfter();
      } else if (spec instanceof KeyPairGeneratorSpec s) {
        if (s.getKeyType() != null && !s.getKeyType().equalsIgnoreCase(algorithm))
          throw new InvalidAlgorithmParameterException("Key type " + s.getKeyType() + " differs from " + algorithm);
        alias = s.getKeystoreAlias(); bits = s.getKeySize(); params = s.getAlgorithmParameterSpec();
        subject = s.getSubjectDN(); serial = s.getSerialNumber();
        notBefore = s.getStartDate(); notAfter = s.getEndDate();
      } else {
        throw new InvalidAlgorithmParameterException(KeyGenParameterSpec.class.getName() + " or "
            + KeyPairGeneratorSpec.class.getName() + " required to initialize this KeyPairGenerator");
      }
      this.random = random == null ? new SecureRandom() : random;
    }
    @Override public KeyPair generateKeyPair() {
      if (alias == null) throw new IllegalStateException("Not initialized");
      try {
        KeyPairGenerator g = standard(algorithm);
        if (algorithm.equals("EC")) {
          g.initialize(params instanceof ECGenParameterSpec ec ? ec : new ECGenParameterSpec(switch (bits) {
            case 224 -> "secp224r1";
            case 384 -> "secp384r1";
            case 521 -> "secp521r1";
            case -1, 256 -> "secp256r1";
            default -> throw new InvalidParameterException("Unsupported EC key size: " + bits);
          }), random);
        } else {
          g.initialize(params instanceof RSAKeyGenParameterSpec rsa ? rsa
              : new RSAKeyGenParameterSpec(bits == -1 ? 2048 : bits, RSAKeyGenParameterSpec.F4), random);
        }
        KeyPair pair = g.generateKeyPair();
        X509Certificate cert = selfSigned(pair, subject, serial, notBefore, notAfter);
        put(alias, new KeyStore.PrivateKeyEntry(pair.getPrivate(), new Certificate[] {cert}));
        return pair;
      } catch (GeneralSecurityException e) {
        throw new ProviderException("Failed to generate key pair", e);
      }
    }
    private static KeyPairGenerator standard(String algorithm) throws NoSuchAlgorithmException {
      for (Provider p : Security.getProviders())
        if (!p.getName().equals(NAME) && p.getService("KeyPairGenerator", algorithm) != null)
          return KeyPairGenerator.getInstance(algorithm, p);
      throw new NoSuchAlgorithmException(algorithm);
    }
  }
  public static final class Rsa extends KeyPairs { public Rsa() { super("RSA"); } }
  public static final class Ec extends KeyPairs { public Ec() { super("EC"); } }

  // A version 1 X.509 certificate signed by the key itself, as Android Keystore makes.
  static X509Certificate selfSigned(KeyPair pair, X500Principal subject, BigInteger serial, Date from, Date to)
      throws GeneralSecurityException {
    boolean rsa = pair.getPrivate().getAlgorithm().equals("RSA");
    byte[] signatureAlgorithm = rsa
        ? der(0x30, new byte[] {6, 9, 0x2A, (byte) 0x86, 0x48, (byte) 0x86, (byte) 0xF7, 0x0D, 1, 1, 0x0B}, new byte[] {5, 0})
        : der(0x30, new byte[] {6, 8, 0x2A, (byte) 0x86, 0x48, (byte) 0xCE, 0x3D, 4, 3, 2});
    byte[] name = subject.getEncoded();
    byte[] tbs = der(0x30, der(2, serial.toByteArray()), signatureAlgorithm, name,
        der(0x30, time(from), time(to)), name, pair.getPublic().getEncoded());
    Signature signer = Signature.getInstance(rsa ? "SHA256withRSA" : "SHA256withECDSA");
    signer.initSign(pair.getPrivate());
    signer.update(tbs);
    byte[] signature = signer.sign();
    byte[] bits = new byte[signature.length + 1];
    System.arraycopy(signature, 0, bits, 1, signature.length);
    byte[] encoded = der(0x30, tbs, signatureAlgorithm, der(3, bits));
    return (X509Certificate) CertificateFactory.getInstance("X.509")
        .generateCertificate(new ByteArrayInputStream(encoded));
  }
  private static byte[] time(Date date) {
    var utc = TimeZone.getTimeZone("UTC");
    var calendar = java.util.Calendar.getInstance(utc, Locale.ROOT);
    calendar.setTime(date);
    int year = calendar.get(java.util.Calendar.YEAR);
    boolean utcTime = year >= 1950 && year < 2050;
    var format = new SimpleDateFormat(utcTime ? "yyMMddHHmmss'Z'" : "yyyyMMddHHmmss'Z'", Locale.ROOT);
    format.setTimeZone(utc);
    return der(utcTime ? 0x17 : 0x18, format.format(date).getBytes(StandardCharsets.US_ASCII));
  }
  private static byte[] der(int tag, byte[]... parts) {
    var content = new ByteArrayOutputStream();
    for (byte[] p : parts) content.writeBytes(p);
    int n = content.size();
    var out = new ByteArrayOutputStream();
    out.write(tag);
    if (n < 0x80) {
      out.write(n);
    } else {
      int bytes = n > 0xFFFFFF ? 4 : n > 0xFFFF ? 3 : n > 0xFF ? 2 : 1;
      out.write(0x80 | bytes);
      for (int i = bytes - 1; i >= 0; i--) out.write(n >>> (8 * i));
    }
    out.writeBytes(content.toByteArray());
    return out.toByteArray();
  }

  // ---- Android cipher names the JDK spells differently ----

  /** Android's PKCS7Padding for AES is PKCS#5 padding; this delegates to the JDK's cipher. */
  public abstract static class Pkcs7 extends CipherSpi {
    private final Cipher cipher;
    private final String mode;
    protected Pkcs7(String mode) {
      this.mode = mode;
      try {
        cipher = Cipher.getInstance("AES/" + mode + "/PKCS5Padding");
      } catch (GeneralSecurityException e) {
        throw new ProviderException(e);
      }
    }
    @Override protected void engineSetMode(String m) throws NoSuchAlgorithmException {
      if (!mode.equalsIgnoreCase(m)) throw new NoSuchAlgorithmException("Unsupported mode " + m);
    }
    @Override protected void engineSetPadding(String padding) throws NoSuchPaddingException {
      if (!padding.equalsIgnoreCase("PKCS7Padding") && !padding.equalsIgnoreCase("PKCS5Padding"))
        throw new NoSuchPaddingException("Unsupported padding " + padding);
    }
    @Override protected int engineGetBlockSize() { return cipher.getBlockSize(); }
    @Override protected int engineGetOutputSize(int inputLen) { return cipher.getOutputSize(inputLen); }
    @Override protected byte[] engineGetIV() { return cipher.getIV(); }
    @Override protected AlgorithmParameters engineGetParameters() { return cipher.getParameters(); }
    @Override protected int engineGetKeySize(Key key) throws InvalidKeyException {
      byte[] encoded = key.getEncoded();
      if (encoded == null) throw new InvalidKeyException("Key has no encoding");
      return encoded.length * 8;
    }
    @Override protected void engineInit(int opmode, Key key, SecureRandom random) throws InvalidKeyException {
      cipher.init(opmode, key, random);
    }
    @Override protected void engineInit(int opmode, Key key, AlgorithmParameterSpec params, SecureRandom random)
        throws InvalidKeyException, InvalidAlgorithmParameterException {
      cipher.init(opmode, key, params, random);
    }
    @Override protected void engineInit(int opmode, Key key, AlgorithmParameters params, SecureRandom random)
        throws InvalidKeyException, InvalidAlgorithmParameterException {
      cipher.init(opmode, key, params, random);
    }
    @Override protected byte[] engineUpdate(byte[] input, int offset, int length) {
      return cipher.update(input, offset, length);
    }
    @Override protected int engineUpdate(byte[] input, int offset, int length, byte[] output, int outputOffset)
        throws ShortBufferException {
      return cipher.update(input, offset, length, output, outputOffset);
    }
    @Override protected byte[] engineDoFinal(byte[] input, int offset, int length)
        throws IllegalBlockSizeException, BadPaddingException {
      return input == null ? cipher.doFinal() : cipher.doFinal(input, offset, length);
    }
    @Override protected int engineDoFinal(byte[] input, int offset, int length, byte[] output, int outputOffset)
        throws ShortBufferException, IllegalBlockSizeException, BadPaddingException {
      return input == null ? cipher.doFinal(output, outputOffset) : cipher.doFinal(input, offset, length, output, outputOffset);
    }
    @Override protected byte[] engineWrap(Key key) throws IllegalBlockSizeException, InvalidKeyException {
      return cipher.wrap(key);
    }
    @Override protected Key engineUnwrap(byte[] wrapped, String algorithm, int type)
        throws InvalidKeyException, NoSuchAlgorithmException {
      return cipher.unwrap(wrapped, algorithm, type);
    }
  }
  public static final class AesCbcPkcs7 extends Pkcs7 { public AesCbcPkcs7() { super("CBC"); } }
  public static final class AesEcbPkcs7 extends Pkcs7 { public AesEcbPkcs7() { super("ECB"); } }
}
