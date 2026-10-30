package android.util;

import java.io.ByteArrayInputStream;
import java.io.FilterInputStream;
import java.io.IOException;
import java.io.InputStream;

/**
 * AOSP android.util.Base64InputStream. The underlying stream is read whole
 * and transformed on the first read; marks are not supported, as in AOSP.
 */
public class Base64InputStream extends FilterInputStream {
  private final int flags;
  private final boolean encode;
  private ByteArrayInputStream output;

  public Base64InputStream(InputStream in, int flags) { this(in, flags, false); }
  public Base64InputStream(InputStream in, int flags, boolean encode) {
    super(in);
    this.flags = flags;
    this.encode = encode;
  }
  private ByteArrayInputStream output() throws IOException {
    if (output == null) {
      byte[] input = in.readAllBytes();
      try {
        output = new ByteArrayInputStream(encode ? Base64.encode(input, flags) : Base64.decode(input, flags));
      } catch (IllegalArgumentException e) {
        throw new IOException("bad base-64", e);
      }
    }
    return output;
  }
  @Override public int read() throws IOException { return output().read(); }
  @Override public int read(byte[] b, int offset, int length) throws IOException { return output().read(b, offset, length); }
  @Override public long skip(long n) throws IOException { return output().skip(n); }
  @Override public int available() throws IOException { return output == null ? 0 : output.available(); }
  @Override public boolean markSupported() { return false; }
  @Override public synchronized void mark(int limit) {}
  @Override public synchronized void reset() throws IOException { throw new IOException("mark/reset not supported"); }
}
