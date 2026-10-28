package android.content.pm;
public final class Signature {
  private final byte[] bytes;
  public Signature(byte[] b) {
    bytes = b.clone();
  }
  public Signature(String hex) {
    bytes = java.util.HexFormat.of().parseHex(hex);
  }
  public byte[] toByteArray() {
    return bytes.clone();
  }
  public String toCharsString() {
    return java.util.HexFormat.of().formatHex(bytes);
  }
  public char[] toChars() {
    return toCharsString().toCharArray();
  }
  public boolean equals(Object o) {
    return o instanceof Signature s && java.util.Arrays.equals(bytes, s.bytes);
  }
  public int hashCode() {
    return java.util.Arrays.hashCode(bytes);
  }
}
