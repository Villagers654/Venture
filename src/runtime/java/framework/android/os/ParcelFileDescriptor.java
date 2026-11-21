package android.os;

import java.io.*;

/**
 * An owned host file for Java streams. Native code asks for a descriptor with
 * getFd or detachFd (Android's way to hand a document to native code); that is
 * a guest descriptor Venture opens on the same host file. adoptFd wraps a guest
 * descriptor native code gave back.
 */
public final class ParcelFileDescriptor implements Closeable, Parcelable {
  // Values from AOSP core/java/android/os/ParcelFileDescriptor.java.
  public static final int MODE_READ_ONLY = 0x10000000, MODE_WRITE_ONLY = 0x20000000,
      MODE_READ_WRITE = 0x30000000, MODE_CREATE = 0x08000000,
      MODE_TRUNCATE = 0x04000000, MODE_APPEND = 0x02000000;
  private final RandomAccessFile file;
  private final FileDescriptor descriptor;
  private final File path;
  private final int mode;
  private int guestFd = -1;
  private boolean closed;
  private ParcelFileDescriptor(RandomAccessFile file, File path, int mode) throws IOException {
    this.file = file;
    this.path = path;
    this.mode = mode;
    descriptor = file.getFD();
  }
  /** A host descriptor this object owns (AOSP's AssetManager natives make these for openFd). */
  public ParcelFileDescriptor(FileDescriptor descriptor) {
    file = null;
    path = null;
    mode = MODE_READ_ONLY;
    this.descriptor = descriptor;
  }
  private ParcelFileDescriptor(int guestFd) {
    file = null;
    path = null;
    mode = MODE_READ_WRITE;
    descriptor = null;
    this.guestFd = guestFd;
  }
  public static ParcelFileDescriptor open(File path, int mode) throws FileNotFoundException {
    int access = mode & MODE_READ_WRITE;
    if (access == 0 || (mode & ~(MODE_READ_WRITE | MODE_CREATE | MODE_TRUNCATE | MODE_APPEND)) != 0)
      throw new IllegalArgumentException("Invalid file mode");
    if (access == MODE_READ_ONLY && (mode & (MODE_CREATE | MODE_TRUNCATE | MODE_APPEND)) != 0)
      throw new IllegalArgumentException("Read-only file cannot be created, truncated or appended");
    if (!path.exists() && (mode & MODE_CREATE) == 0) throw new FileNotFoundException(path.toString());
    // Java opens a writable file for reading too; a write-only descriptor's
    // owner simply never reads it.
    RandomAccessFile file = new RandomAccessFile(path, access == MODE_READ_ONLY ? "r" : "rw");
    try {
      if ((mode & MODE_TRUNCATE) != 0) file.setLength(0);
      if ((mode & MODE_APPEND) != 0) file.seek(file.length());
      return new ParcelFileDescriptor(file, path, mode);
    } catch (IOException e) {
      try { file.close(); } catch (IOException close) { e.addSuppressed(close); }
      FileNotFoundException failure = new FileNotFoundException(e.getMessage());
      failure.initCause(e); throw failure;
    }
  }
  /** AOSP ParcelFileDescriptor.parseMode: "r", "w", "wt", "wa", "rw", "rwt". */
  public static int parseMode(String mode) {
    return switch (mode == null ? "" : mode) {
      case "r" -> MODE_READ_ONLY;
      case "w", "wt" -> MODE_WRITE_ONLY | MODE_CREATE | MODE_TRUNCATE;
      case "wa" -> MODE_WRITE_ONLY | MODE_CREATE | MODE_APPEND;
      case "rw" -> MODE_READ_WRITE | MODE_CREATE;
      case "rwt" -> MODE_READ_WRITE | MODE_CREATE | MODE_TRUNCATE;
      default -> throw new IllegalArgumentException("Bad mode '" + mode + "'");
    };
  }
  /** A guest descriptor native code owns, from detachFd or its own open. */
  public static ParcelFileDescriptor adoptFd(int fd) { return new ParcelFileDescriptor(fd); }
  public FileDescriptor getFileDescriptor() { return descriptor; }
  public synchronized long getStatSize() {
    try { return closed || file == null ? -1 : file.length(); } catch (IOException e) { return -1; }
  }
  /** The guest descriptor for this file, opened on first use; this object still owns it. */
  public synchronized int getFd() {
    if (closed) throw new IllegalStateException("Already closed");
    if (guestFd < 0 && path == null) throw new UnsupportedOperationException("Host descriptor has no guest descriptor");
    if (guestFd < 0) {
      // Linux open(2) flags: O_RDONLY 0, O_WRONLY 1, O_RDWR 2, O_APPEND 02000.
      int access = mode & MODE_READ_WRITE;
      int flags = access == MODE_READ_ONLY ? 0 : access == MODE_WRITE_ONLY ? 1 : 2;
      if ((mode & MODE_APPEND) != 0) flags |= 02000;
      int fd = com.venture.runtime.RuntimeEnvironment.host().openDocumentFd(path.getPath(), flags);
      if (fd < 0) throw new IllegalStateException("Cannot open a guest descriptor for " + path.getName() + ": " + fd);
      guestFd = fd;
    }
    return guestFd;
  }
  /** Hands the guest descriptor to the caller, who closes it; this object is closed. */
  public synchronized int detachFd() {
    int fd = getFd();
    guestFd = -1;
    try { close(); } catch (IOException ignored) { /* the detached descriptor stays open */ }
    return fd;
  }
  public ParcelFileDescriptor dup() throws IOException {
    if (path == null) throw new IOException("A native descriptor cannot be duplicated");
    return open(path, mode & ~(MODE_CREATE | MODE_TRUNCATE));
  }
  public synchronized void close() throws IOException {
    if (closed) return;
    closed = true;
    try {
      if (file != null) file.close();
      else if (descriptor != null && descriptor.valid()) new FileInputStream(descriptor).close();
    } finally {
      if (guestFd >= 0) com.venture.runtime.RuntimeEnvironment.host().closeGuestFd(guestFd);
      guestFd = -1;
    }
  }
  /** AOSP's hidden seekTo (AssetFileDescriptor's bounded streams). */
  public synchronized long seekTo(long position) throws IOException {
    if (file == null) throw new IOException("A native descriptor cannot seek here");
    file.seek(position);
    return position;
  }
  public void closeWithError(String message) throws IOException { close(); }
  // Raw host descriptors (sockets, pipes, a bitmap's or fence's buffer) have no
  // guest descriptor here.
  public static ParcelFileDescriptor fromFd(int fd) throws IOException {
    throw new IOException("Native descriptor import unavailable");
  }
  public static ParcelFileDescriptor dup(FileDescriptor descriptor) throws IOException {
    throw new IOException("Native descriptor import unavailable");
  }
  public int describeContents() { return CONTENTS_FILE_DESCRIPTOR; }
  public void writeToParcel(Parcel dest, int flags) {
    throw new UnsupportedOperationException("File descriptor parcel transfer unavailable");
  }
  public static final Parcelable.Creator<ParcelFileDescriptor> CREATOR = new Parcelable.Creator<>() {
    @Override public ParcelFileDescriptor createFromParcel(Parcel in) {
      throw new UnsupportedOperationException("File descriptor parcel transfer unavailable");
    }
    @Override public ParcelFileDescriptor[] newArray(int size) { return new ParcelFileDescriptor[size]; }
  };
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
