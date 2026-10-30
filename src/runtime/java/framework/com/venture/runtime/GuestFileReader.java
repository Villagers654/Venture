package com.venture.runtime;

import java.io.*;
import java.lang.ref.Cleaner;
import java.nio.charset.Charset;

public final class GuestFileReader extends FileReader {
  private Cleaner.Cleanable cleanup;
  public GuestFileReader(String path) throws FileNotFoundException { this(new File(GuestFiles.host(path))); }
  public GuestFileReader(File file) throws FileNotFoundException { this(GuestFiles.open(file)); }
  private GuestFileReader(GuestFiles.Snapshot snapshot) throws FileNotFoundException {
    super(snapshot.file);
    cleanup = snapshot.track(this);
  }
  public GuestFileReader(FileDescriptor descriptor) { super(descriptor); }
  public GuestFileReader(String path, Charset charset) throws IOException { this(new File(GuestFiles.host(path)), charset); }
  public GuestFileReader(File file, Charset charset) throws IOException { this(GuestFiles.open(file), charset); }
  private GuestFileReader(GuestFiles.Snapshot snapshot, Charset charset) throws IOException {
    super(snapshot.file, charset);
    cleanup = snapshot.track(this);
  }
  public void close() throws IOException {
    try { super.close(); } finally { if (cleanup != null) cleanup.clean(); }
  }
}
