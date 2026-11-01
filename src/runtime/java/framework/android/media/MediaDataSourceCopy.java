package android.media;
/** Copies a MediaDataSource to a deleted-on-close temporary file for the file-based host decoder. */
final class MediaDataSourceCopy {
  private MediaDataSourceCopy() {}
  static java.io.FileInputStream open(MediaDataSource source) throws java.io.IOException {
    var file = java.io.File.createTempFile("venture-media", ".bin");
    file.deleteOnExit();
    try (var out = new java.io.FileOutputStream(file)) {
      long size = source.getSize(), position = 0;
      byte[] buffer = new byte[1 << 16];
      while (size < 0 || position < size) {
        int want = size < 0 ? buffer.length : (int) Math.min(buffer.length, size - position);
        int read = source.readAt(position, buffer, 0, want);
        if (read <= 0) break;
        out.write(buffer, 0, read);
        position += read;
      }
    } catch (java.io.IOException | RuntimeException failure) {
      file.delete();
      throw failure;
    }
    var stream = new java.io.FileInputStream(file);
    file.delete();  // Windows keeps an open file; deleteOnExit covers it there.
    return stream;
  }
}
