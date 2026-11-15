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
    // Android's /etc links to /system/etc.
    if (path.equals("/etc") || path.startsWith("/etc/")) path = "/system" + path;
    return path.equals("/proc") || path.startsWith("/proc/") || path.equals("/sys") || path.startsWith("/sys/")
        || systemFontsPath(path) ? path : null;
  }
  // The guest's system font directory, its files and fonts.xml (hle/libc/system_fonts.h).
  private static boolean systemFontsPath(String path) {
    return path.equals(FONTS) || path.startsWith(FONTS + "/") || path.equals("/system/etc")
        || path.equals("/system/etc/fonts.xml");
  }
  private static final String FONTS = "/system/fonts";
  /** The installed host font an Android /system/fonts/<name> path stands for, or null. */
  public static String systemFont(String path) {
    if (path == null || !path.startsWith(FONTS + "/")) return null;
    try {
      return RuntimeEnvironment.host().systemFont(path);
    } catch (UnsatisfiedLinkError noRuntime) {  // the framework outside the runtime
      return null;
    }
  }
  /** Android absolute paths of the application's mounts as host paths (RuntimeEnvironment.toHostPath). */
  public static String host(String path) { return RuntimeEnvironment.toHostPath(path); }
  public static boolean exists(File file) {
    String path = virtualPath(file);
    return path == null ? file.exists()
        : RuntimeEnvironment.host().virtualFileExists(path.getBytes(StandardCharsets.UTF_8));
  }
  /** Android's synthesized directories (/system/fonts, /system/etc) are directories here too. */
  public static boolean isDirectory(File file) {
    String path = virtualPath(file);
    if (path == null || !(path.equals(FONTS) || path.equals("/system/etc"))) return file.isDirectory();
    return RuntimeEnvironment.host().virtualFileExists(path.getBytes(StandardCharsets.UTF_8));
  }
  /**
   * Android's File(parent, child) joins even an absolute child under the parent. App paths are
   * host paths here, and WinNTFileSystem would join a drive-qualified child as "parent\C:\...",
   * so the child's drive is dropped and the rest joins under the parent as on Android.
   */
  public static String child(Object parent, String child) {
    if (parent == null || child == null) return child;
    if (child.length() >= 3 && child.charAt(1) == ':') {
      char drive = child.charAt(0), slash = child.charAt(2);
      boolean letter = (drive >= 'A' && drive <= 'Z') || (drive >= 'a' && drive <= 'z');
      if (letter && (slash == '\\' || slash == '/')) return child.substring(2);
    }
    // Android names may hold characters Windows forbids (Firebase's "prefs:<id>").
    return RuntimeEnvironment.hostName(child);
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
    String font = systemFont(path);
    if (font != null) return new Snapshot(new File(font), false);
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
