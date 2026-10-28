package com.venture.runtime;
import android.content.pm.Signature;
import java.io.*;
import java.nio.*;
import java.security.cert.CertificateFactory;
import java.util.*;
import java.util.zip.*;

/**
 * Extracts certificate identity, not an entitlement or APK authenticity verdict.
 * APK Signature Scheme v2/v3 framing: platform/tools/apksig ApkSigningBlockUtils.
 */
public final class ApkSignatures {
  private static ByteBuffer slice(ByteBuffer b) throws IOException {
    if (b.remaining() < 4)
      throw new IOException("Truncated signer");
    int n = b.getInt();
    if (n < 0 || n > b.remaining())
      throw new IOException("Invalid signer length");
    ByteBuffer s = b.slice().order(ByteOrder.LITTLE_ENDIAN);
    s.limit(n);
    b.position(b.position() + n);
    return s;
  }
  public static Signature[] read(File apk) throws IOException {
    try (RandomAccessFile f = new RandomAccessFile(apk, "r")) {
      int tailSize = (int) Math.min(f.length(), 65557);
      byte[] tail = new byte[tailSize];
      f.seek(f.length() - tailSize);
      f.readFully(tail);
      ByteBuffer end = ByteBuffer.wrap(tail).order(ByteOrder.LITTLE_ENDIAN);
      for (int p = tailSize - 22; p >= 0; p--)
        if (end.getInt(p) == 0x06054b50
            && p + 22 + Short.toUnsignedInt(end.getShort(p + 20)) == tailSize) {
          long cd = Integer.toUnsignedLong(end.getInt(p + 16));
          if (cd >= 24) {
            byte[] footer = new byte[24];
            f.seek(cd - 24);
            f.readFully(footer);
            ByteBuffer b = ByteBuffer.wrap(footer).order(ByteOrder.LITTLE_ENDIAN);
            long size = b.getLong();
            if (new String(footer, 8, 16, java.nio.charset.StandardCharsets.US_ASCII)
                    .equals("APK Sig Block 42")) {
              if (size < 24 || size > 16 * 1024 * 1024 || size + 8 > cd)
                throw new IOException("Invalid APK signing block");
              byte[] block = new byte[(int) size + 8];
              f.seek(cd - size - 8);
              f.readFully(block);
              b = ByteBuffer.wrap(block).order(ByteOrder.LITTLE_ENDIAN);
              if (b.getLong() != size)
                throw new IOException("Signing block size mismatch");
              b.limit(block.length - 24);
              while (b.hasRemaining()) {
                if (b.remaining() < 8)
                  throw new IOException("Truncated signing pair");
                long n = b.getLong();
                if (n < 4 || n > b.remaining())
                  throw new IOException("Invalid signing pair");
                int stop = b.position() + (int) n;
                int id = b.getInt();
                if (id == 0x7109871a || id == 0xf05368c0) {
                  ByteBuffer pair = b.slice().order(ByteOrder.LITTLE_ENDIAN);
                  pair.limit((int) n - 4);
                  ByteBuffer signers = slice(pair);
                  List<Signature> signatures = new ArrayList<>();
                  while (signers.hasRemaining()) {
                    ByteBuffer signer = slice(signers), signed = slice(signer);
                    slice(signed);
                    ByteBuffer certs = slice(signed);
                    ByteBuffer certificate = slice(certs);
                    byte[] cert = new byte[certificate.remaining()];
                    certificate.get(cert);
                    signatures.add(new Signature(cert));
                  }
                  if (!signatures.isEmpty())
                    return signatures.toArray(Signature[] ::new);
                }
                b.position(stop);
              }
            }
          }
          break;
        }
    } catch (BufferUnderflowException | IllegalArgumentException e) {
      throw new IOException("Malformed APK signature block", e);
    }
    try (ZipFile zip = new ZipFile(apk)) {
      List<Signature> signatures = new ArrayList<>();
      for (var entry : Collections.list(zip.entries()))
        if (entry.getName().matches("META-INF/[^/]+\\.(RSA|DSA|EC)")) {
          try (InputStream in = zip.getInputStream(entry)) {
            var certs = CertificateFactory.getInstance("X.509").generateCertificates(in);
            if (!certs.isEmpty())
              signatures.add(new Signature(certs.iterator().next().getEncoded()));
          } catch (java.security.cert.CertificateException e) {
            throw new IOException(e);
          }
        }
      if (signatures.isEmpty())
        throw new IOException("APK has no supported signing certificate");
      return signatures.toArray(Signature[] ::new);
    }
  }
}
