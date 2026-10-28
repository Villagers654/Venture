package com.venture.runtime;

import java.io.*;
import java.lang.ref.Cleaner;
import java.nio.file.*;
import java.nio.charset.StandardCharsets;

/** The APK's java.io reads of proc/sys use the same virtual files as bionic. */
public final class GuestFiles {
  private static final Cleaner CLEANER = Cleaner.create();
  private static String virtualPath(File file) {
    // WinNTFileSystem normalizes a leading Android slash to a backslash.
    String path = file.getPath().replace('\\', '/');
    return path.equals("/proc") || path.startsWith("/proc/") || path.equals("/sys") || path.startsWith("/sys/")
        ? path : null;
  }
  public static boolean exists(File file) {
    String path = virtualPath(file);
    return path == null ? file.exists()
        : RuntimeEnvironment.host().virtualFileExists(path.getBytes(StandardCharsets.UTF_8));
  }
  static final class Snapshot implements Runnable {
    final File file;
    final boolean temporary;
    Snapshot(File file, boolean temporary) { this.file = file; this.temporary = temporary; }
    Cleaner.Cleanable track(Object owner) { return CLEANER.register(owner, this); }
    public void run() {
      if (temporary && file.exists() && !file.delete())
        RuntimeEnvironment.host().log(5, "GuestFiles", "Virtual read snapshot cleanup deferred until JVM exit");
    }
  }
  static Snapshot open(File file) throws FileNotFoundException {
    String path = virtualPath(file);
    if (path == null) return new Snapshot(file, false);
    byte[] bytes = RuntimeEnvironment.host().readVirtualFile(path.getBytes(StandardCharsets.UTF_8));
    if (bytes == null) throw new FileNotFoundException(path);
    Path temporary = null;
    try {
      // Each open sees a fresh proc snapshot; concurrent readers keep their
      // own offset and contents. Ordinary JDK streams retain decoding/FD APIs.
      temporary = Files.createTempFile("venture-proc-", ".tmp");
      temporary.toFile().deleteOnExit();
      Files.write(temporary, bytes);
      return new Snapshot(temporary.toFile(), true);
    } catch (IOException failure) {
      if (temporary != null) try { Files.deleteIfExists(temporary); } catch (IOException ignored) {}
      var error = new FileNotFoundException("Cannot open virtual filesystem snapshot");
      error.initCause(failure);
      throw error;
    }
  }
}
