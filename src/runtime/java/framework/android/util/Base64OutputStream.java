package android.util;

import java.io.ByteArrayOutputStream;
import java.io.FilterOutputStream;
import java.io.IOException;
import java.io.OutputStream;

/**
 * AOSP android.util.Base64OutputStream. The bytes are transformed when the
 * stream closes, which yields the same output as AOSP's incremental coder;
 * Base64.NO_CLOSE leaves the underlying stream open.
 */
public class Base64OutputStream extends FilterOutputStream {
  private final int flags;
  private final boolean encode;
  private final ByteArrayOutputStream pending = new ByteArrayOutputStream();
  private boolean closed;

  public Base64OutputStream(OutputStream out, int flags) { this(out, flags, true); }
  public Base64OutputStream(OutputStream out, int flags, boolean encode) {
    super(out);
    this.flags = flags;
    this.encode = encode;
  }
  @Override public void write(int b) throws IOException {
    if (closed) throw new IOException("Stream closed");
    pending.write(b);
  }
  @Override public void write(byte[] b, int offset, int length) throws IOException {
    if (closed) throw new IOException("Stream closed");
    pending.write(b, offset, length);
  }
  @Override public void close() throws IOException {
    if (closed) return;
    closed = true;
    byte[] input = pending.toByteArray();
    try {
      out.write(encode ? Base64.encode(input, flags) : Base64.decode(input, flags));
    } catch (IllegalArgumentException e) {
      throw new IOException("bad base-64", e);
    }
    if ((flags & Base64.NO_CLOSE) != 0) out.flush();
    else out.close();
  }
}
