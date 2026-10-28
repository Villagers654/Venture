package android.util;

import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.Objects;

/** Android 12L Base64 flags and line termination, backed by the JDK codec. */
public final class Base64 {
  public static final int DEFAULT = 0, NO_PADDING = 1, NO_WRAP = 2, CRLF = 4, URL_SAFE = 8, NO_CLOSE = 16;
  private Base64() {}
  public static byte[] decode(String text, int flags) {
    return decode(text.getBytes(StandardCharsets.UTF_8), flags);
  }
  public static byte[] decode(byte[] input, int flags) { return decode(input, 0, input.length, flags); }
  public static byte[] decode(byte[] input, int offset, int length, int flags) {
    Objects.checkFromIndexSize(offset, length, input.length);
    boolean url = (flags & URL_SAFE) != 0;
    byte[] filtered = new byte[length];
    int count = 0;
    // AOSP core/java/android/util/Base64.java: DECODE/DECODE_WEBSAFE mark
    // non-alphabet bytes SKIP, including whitespace; '=' remains structural.
    for (int i = offset; i < offset + length; i++) {
      int c = input[i] & 255;
      if (c >= 'A' && c <= 'Z' || c >= 'a' && c <= 'z' || c >= '0' && c <= '9'
          || c == '=' || c == (url ? '-' : '+') || c == (url ? '_' : '/'))
        filtered[count++] = (byte) c;
    }
    return (url ? java.util.Base64.getUrlDecoder() : java.util.Base64.getDecoder())
        .decode(Arrays.copyOf(filtered, count));
  }
  public static byte[] encode(byte[] input, int flags) { return encode(input, 0, input.length, flags); }
  public static byte[] encode(byte[] input, int offset, int length, int flags) {
    Objects.checkFromIndexSize(offset, length, input.length);
    var encoder = (flags & URL_SAFE) != 0 ? java.util.Base64.getUrlEncoder() : java.util.Base64.getEncoder();
    if ((flags & NO_PADDING) != 0) encoder = encoder.withoutPadding();
    byte[] data = encoder.encode(Arrays.copyOfRange(input, offset, offset + length));
    if ((flags & NO_WRAP) != 0 || data.length == 0) return data;
    // Android terminates even the last nonempty line. JDK MIME omits that LF.
    int newline = (flags & CRLF) != 0 ? 2 : 1;
    byte[] wrapped = new byte[data.length + ((data.length + 75) / 76) * newline];
    int out = 0;
    for (int start = 0; start < data.length; start += 76) {
      int n = Math.min(76, data.length - start);
      System.arraycopy(data, start, wrapped, out, n);
      out += n;
      if (newline == 2) wrapped[out++] = '\r';
      wrapped[out++] = '\n';
    }
    return wrapped;
  }
  public static String encodeToString(byte[] input, int flags) {
    return new String(encode(input, flags), StandardCharsets.US_ASCII);
  }
  public static String encodeToString(byte[] input, int offset, int length, int flags) {
    return new String(encode(input, offset, length, flags), StandardCharsets.US_ASCII);
  }
}
