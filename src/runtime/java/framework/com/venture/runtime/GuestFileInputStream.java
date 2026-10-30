package com.venture.runtime;

import java.io.*;
import java.lang.ref.Cleaner;

public final class GuestFileInputStream extends FileInputStream {
  private Cleaner.Cleanable cleanup;
  private final java.util.concurrent.atomic.AtomicBoolean closing = new java.util.concurrent.atomic.AtomicBoolean();
  public GuestFileInputStream(String path) throws FileNotFoundException { this(new File(GuestFiles.host(path))); }
  public GuestFileInputStream(File file) throws FileNotFoundException { this(GuestFiles.open(file)); }
  private GuestFileInputStream(GuestFiles.Snapshot snapshot) throws FileNotFoundException {
    super(snapshot.file);
    cleanup = snapshot.track(this);
  }
  public GuestFileInputStream(FileDescriptor descriptor) { super(descriptor); }
  public void close() throws IOException {
    // FileInputStream.close closes its FileChannel, which calls the parent's
    // close again before the descriptor is closed. Delete only at outer exit.
    if (!closing.compareAndSet(false, true)) return;
    try { super.close(); } finally { if (cleanup != null) cleanup.clean(); }
  }
}
