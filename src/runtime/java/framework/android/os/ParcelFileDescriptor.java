package android.os;

import java.io.*;

/** Owned host file for Java streams. Raw guest descriptor import/export is unavailable. */
public final class ParcelFileDescriptor implements Closeable, Parcelable {
  // Values from AOSP core/java/android/os/ParcelFileDescriptor.java.
  public static final int MODE_READ_ONLY = 0x10000000, MODE_WRITE_ONLY = 0x20000000,
      MODE_READ_WRITE = 0x30000000, MODE_CREATE = 0x08000000,
      MODE_TRUNCATE = 0x04000000, MODE_APPEND = 0x02000000;
  private final RandomAccessFile file;
  private final FileDescriptor descriptor;
  private boolean closed;
  private ParcelFileDescriptor(RandomAccessFile file) throws IOException {
    this.file = file;
    descriptor = file.getFD();
  }
  public static ParcelFileDescriptor open(File path, int mode) throws FileNotFoundException {
    int access = mode & MODE_READ_WRITE;
    if (access == 0 || (mode & ~(MODE_READ_WRITE | MODE_CREATE | MODE_TRUNCATE | MODE_APPEND)) != 0)
      throw new IllegalArgumentException("Invalid file mode");
    if (access == MODE_WRITE_ONLY || (mode & MODE_APPEND) != 0)
      throw new UnsupportedOperationException("Write-only/append descriptor access unavailable");
    if (access == MODE_READ_ONLY && (mode & (MODE_CREATE | MODE_TRUNCATE | MODE_APPEND)) != 0)
      throw new IllegalArgumentException("Read-only file cannot be created, truncated or appended");
    if (!path.exists() && (mode & MODE_CREATE) == 0) throw new FileNotFoundException(path.toString());
    RandomAccessFile file = new RandomAccessFile(path, access == MODE_READ_ONLY ? "r" : "rw");
    try {
      if ((mode & MODE_TRUNCATE) != 0) file.setLength(0);
      return new ParcelFileDescriptor(file);
    } catch (IOException e) {
      try { file.close(); } catch (IOException close) { e.addSuppressed(close); }
      FileNotFoundException failure = new FileNotFoundException(e.getMessage());
      failure.initCause(e); throw failure;
    }
  }
  public FileDescriptor getFileDescriptor() { return descriptor; }
  public synchronized long getStatSize() {
    try { return closed ? -1 : file.length(); } catch (IOException e) { return -1; }
  }
  public int getFd() { throw new UnsupportedOperationException("Host Java file has no guest descriptor"); }
  public synchronized void close() throws IOException { if (!closed) { closed = true; file.close(); } }
  public int describeContents() { return CONTENTS_FILE_DESCRIPTOR; }
  public void writeToParcel(Parcel dest, int flags) {
    throw new UnsupportedOperationException("File descriptor parcel transfer unavailable");
  }
  public static class AutoCloseInputStream extends FileInputStream {
    private final ParcelFileDescriptor owner;
    public AutoCloseInputStream(ParcelFileDescriptor pfd) { super(pfd.getFileDescriptor()); owner = pfd; }
    public void close() throws IOException { try { super.close(); } finally { owner.close(); } }
  }
  public static class AutoCloseOutputStream extends FileOutputStream {
    private final ParcelFileDescriptor owner;
    public AutoCloseOutputStream(ParcelFileDescriptor pfd) { super(pfd.getFileDescriptor()); owner = pfd; }
    public void close() throws IOException { try { super.close(); } finally { owner.close(); } }
  }
}
