package dalvik.system;

import java.io.*;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.zip.ZipFile;

/**
 * Deprecated AOSP API, kept for apps that list their own classes (Talsec). Reads class names from a
 * .dex file or every classes*.dex in an APK; it never loads code.
 */
@Deprecated
public final class DexFile {
  private final String name;
  private final List<String> classes = new ArrayList<>();
  public DexFile(File file) throws IOException { this(file.getPath()); }
  public DexFile(String fileName) throws IOException {
    name = fileName;
    byte[] head;
    try (var in = new FileInputStream(fileName)) { head = in.readNBytes(4); }
    if (head.length == 4 && head[0] == 'd' && head[1] == 'e' && head[2] == 'x') {
      try (var in = new FileInputStream(fileName)) { read(in.readAllBytes()); }
      return;
    }
    try (var zip = new ZipFile(fileName)) {
      for (int i = 1;; ++i) {
        var entry = zip.getEntry(i == 1 ? "classes.dex" : "classes" + i + ".dex");
        if (entry == null) break;
        try (var in = zip.getInputStream(entry)) { read(in.readAllBytes()); }
      }
    }
    if (classes.isEmpty()) throw new IOException("No dex files in " + fileName);
  }
  private void read(byte[] bytes) throws IOException {
    try {
      var dex = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);
      int strings = dex.getInt(0x3c), types = dex.getInt(0x44);
      int count = dex.getInt(0x60), defs = dex.getInt(0x64);
      for (int i = 0; i < count; ++i) {
        int type = dex.getInt(defs + i * 32);
        int string = dex.getInt(types + type * 4);
        int at = dex.getInt(strings + string * 4);
        while ((bytes[at++] & 0x80) != 0) {}  // ULEB128 UTF-16 length
        int end = at;
        while (bytes[end] != 0) ++end;
        String descriptor = new String(bytes, at, end - at, StandardCharsets.UTF_8);
        if (descriptor.startsWith("L") && descriptor.endsWith(";"))
          classes.add(descriptor.substring(1, descriptor.length() - 1).replace('/', '.'));
      }
    } catch (IndexOutOfBoundsException e) {
      throw new IOException("Malformed dex file " + name, e);
    }
  }
  public Enumeration<String> entries() { return Collections.enumeration(classes); }
  public String getName() { return name; }
  public void close() throws IOException {}
  public static boolean isDexOptNeeded(String fileName) { return false; }
  @Override public String toString() { return name; }
}
