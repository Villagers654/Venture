package com.venture.runtime;
public final class SystemAssets {
  private static final class Table {
    static final ResourceTable INSTANCE = new ResourceTable(read("resources.arsc"));
  }
  public static ResourceTable table() {
    return Table.INSTANCE;
  }
  public static byte[] read(String path) {
    if (path.contains("..") || path.startsWith("/"))
      throw new IllegalArgumentException("Invalid system resource path");
    try (var stream = SystemAssets.class.getResourceAsStream("/venture/android-32/" + path)) {
      if (stream == null)
        throw new android.content.res.Resources.NotFoundException("System resource " + path);
      return stream.readAllBytes();
    } catch (java.io.IOException e) {
      throw new IllegalStateException(e);
    }
  }
}
