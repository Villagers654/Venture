package android.content.res;

import android.os.*;
import java.io.*;
import java.util.Objects;

/** A bounded region of an owned file, following AOSP AssetFileDescriptor stream semantics. */
public class AssetFileDescriptor implements Closeable, Parcelable {
  public static final long UNKNOWN_LENGTH = -1;
  private final ParcelFileDescriptor descriptor;
  private final long start, length;
  private final Bundle extras;
  public AssetFileDescriptor(ParcelFileDescriptor descriptor, long startOffset, long length) {
    this(descriptor, startOffset, length, null);
  }
  public AssetFileDescriptor(ParcelFileDescriptor descriptor, long startOffset, long length, Bundle extras) {
    this.descriptor = Objects.requireNonNull(descriptor);
    if (startOffset < 0 || length < -1 || (length == UNKNOWN_LENGTH && startOffset != 0))
      throw new IllegalArgumentException("Invalid asset region");
    this.start = startOffset; this.length = length;
    this.extras = extras == null ? null : new Bundle(extras);
  }
  public ParcelFileDescriptor getParcelFileDescriptor() { return descriptor; }
  public FileDescriptor getFileDescriptor() { return descriptor.getFileDescriptor(); }
  public long getStartOffset() { return start; }
  public long getDeclaredLength() { return length; }
  public long getLength() { return length == UNKNOWN_LENGTH ? descriptor.getStatSize() : length; }
  public Bundle getExtras() { return extras; }
  public void close() throws IOException { descriptor.close(); }
  public FileInputStream createInputStream() throws IOException { return new AutoCloseInputStream(this); }
  public FileOutputStream createOutputStream() throws IOException { return new AutoCloseOutputStream(this); }
  public int describeContents() { return CONTENTS_FILE_DESCRIPTOR; }
  public void writeToParcel(Parcel dest, int flags) {
    throw new UnsupportedOperationException("File descriptor parcel transfer unavailable");
  }
  public static class AutoCloseInputStream extends ParcelFileDescriptor.AutoCloseInputStream {
    private long remaining;
    public AutoCloseInputStream(AssetFileDescriptor asset) throws IOException {
      super(asset.descriptor); getChannel().position(asset.start); remaining = asset.length;
    }
    public int available() throws IOException {
      return remaining < 0 ? super.available() : (int)Math.min(super.available(), Math.min(remaining, Integer.MAX_VALUE));
    }
    public int read() throws IOException {
      if (remaining == 0) return -1;
      int value = super.read(); if (value >= 0 && remaining > 0) --remaining; return value;
    }
    public int read(byte[] b) throws IOException { return read(b, 0, b.length); }
    public int read(byte[] b, int off, int len) throws IOException {
      Objects.checkFromIndexSize(off, len, b.length);
      if (len == 0) return 0;
      if (remaining == 0) return -1;
      int count = super.read(b, off, remaining < 0 ? len : (int)Math.min(len, remaining));
      if (count > 0 && remaining > 0) remaining -= count;
      return count;
    }
    public long skip(long count) throws IOException {
      long skipped = super.skip(remaining < 0 ? Math.max(0, count) : Math.min(remaining, Math.max(0, count)));
      if (remaining > 0) remaining -= skipped;
      return skipped;
    }
    // HotSpot FileInputStream's optimized methods bypass read(byte[], int, int).
    public byte[] readAllBytes() throws IOException { return readNBytes(Integer.MAX_VALUE); }
    public byte[] readNBytes(int count) throws IOException {
      if (count < 0) throw new IllegalArgumentException("Negative byte count");
      ByteArrayOutputStream result = new ByteArrayOutputStream();
      byte[] buffer = new byte[Math.min(8192, count)];
      while (count > 0) {
        int got = read(buffer, 0, Math.min(buffer.length, count));
        if (got < 0) break;
        result.write(buffer, 0, got); count -= got;
      }
      return result.toByteArray();
    }
    public long transferTo(OutputStream output) throws IOException {
      Objects.requireNonNull(output); byte[] buffer = new byte[8192]; long count = 0;
      for (int got; (got = read(buffer)) >= 0;) { output.write(buffer, 0, got); count += got; }
      return count;
    }
  }
  public static class AutoCloseOutputStream extends ParcelFileDescriptor.AutoCloseOutputStream {
    private long remaining;
    public AutoCloseOutputStream(AssetFileDescriptor asset) throws IOException {
      super(asset.descriptor); getChannel().position(asset.start); remaining = asset.length;
    }
    public void write(int value) throws IOException {
      if (remaining == 0) return;
      super.write(value); if (remaining > 0) --remaining;
    }
    public void write(byte[] b) throws IOException { write(b, 0, b.length); }
    public void write(byte[] b, int off, int len) throws IOException {
      Objects.checkFromIndexSize(off, len, b.length);
      int count = remaining < 0 ? len : (int)Math.min(len, remaining);
      super.write(b, off, count); if (remaining > 0) remaining -= count;
    }
  }
}
