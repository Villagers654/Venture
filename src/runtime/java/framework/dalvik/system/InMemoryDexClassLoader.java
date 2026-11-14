package dalvik.system;
// API 26+: classes from DEX bytes in memory (the Android Game SDK loads Swappy's
// Java classes this way). Each buffer's remaining bytes are one DEX, converted
// like a DEX file on BaseDexClassLoader's path; the buffers' positions stay.
public final class InMemoryDexClassLoader extends BaseDexClassLoader {
  public InMemoryDexClassLoader(java.nio.ByteBuffer[] dexBuffers, String librarySearchPath, ClassLoader parent) {
    this(stage(dexBuffers), librarySearchPath, parent);
  }
  public InMemoryDexClassLoader(java.nio.ByteBuffer[] dexBuffers, ClassLoader parent) {
    this(dexBuffers, null, parent);
  }
  public InMemoryDexClassLoader(java.nio.ByteBuffer dexBuffer, ClassLoader parent) {
    this(new java.nio.ByteBuffer[] {dexBuffer}, parent);
  }
  private InMemoryDexClassLoader(java.io.File[] staged, String librarySearchPath, ClassLoader parent) {
    super(path(staged), null, librarySearchPath, parent);
    for (var file : staged)
      file.delete();  // converted in the constructor; the cache keeps the classes
  }
  private static java.io.File[] stage(java.nio.ByteBuffer[] buffers) {
    if (buffers == null)
      throw new NullPointerException("dexBuffers == null");
    var files = new java.io.File[buffers.length];
    try {
      for (int i = 0; i < buffers.length; i++) {
        if (buffers[i] == null)
          throw new NullPointerException("dexBuffers[" + i + "] == null");
        files[i] = java.io.File.createTempFile("venture-memory", ".dex");
        try (var out = java.nio.channels.FileChannel.open(files[i].toPath(), java.nio.file.StandardOpenOption.WRITE)) {
          var bytes = buffers[i].duplicate();
          while (bytes.hasRemaining())
            out.write(bytes);
        }
      }
      return files;
    } catch (java.io.IOException e) {
      for (var file : files)
        if (file != null) file.delete();
      throw new java.io.UncheckedIOException(e);
    }
  }
  private static String path(java.io.File[] files) {
    var path = new StringBuilder();
    for (var file : files) {
      if (path.length() > 0) path.append(java.io.File.pathSeparatorChar);
      path.append(file.getPath());
    }
    return path.toString();
  }
}
