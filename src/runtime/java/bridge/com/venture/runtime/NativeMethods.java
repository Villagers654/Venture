package com.venture.runtime;

import java.io.*;
import java.util.ArrayList;

/** Reads JNI declarations without resolving unrelated Java method parameter types. */
public final class NativeMethods {
  private NativeMethods() {}
  // ART calls a static native annotated @CriticalNative without JNIEnv* or jclass
  // (API 26+). The annotation has build retention, so it is read from the class file.
  private static final String CRITICAL = "Ldalvik/annotation/optimization/CriticalNative;";
  /** Name and descriptor pairs of every native method. */
  public static String[] declarations(Class<?> cls) {
    return read(cls, false);
  }
  /** Name and descriptor pairs of the static natives annotated @CriticalNative. */
  public static String[] criticalNatives(Class<?> cls) {
    return read(cls, true);
  }
  private static String[] read(Class<?> cls, boolean criticalOnly) {
    String resource = "/" + cls.getName().replace('.', '/') + ".class";
    try (InputStream source = cls.getResourceAsStream(resource)) {
      if (source == null) throw new IOException("Class bytes unavailable: " + cls.getName());
      DataInputStream input = new DataInputStream(source);
      if (input.readInt() != 0xcafebabe) throw new IOException("Invalid class file");
      input.skipNBytes(4); // minor_version, major_version
      // JVMS 4.4: only CONSTANT_Utf8 is needed for native names/descriptors.
      String[] utf = new String[input.readUnsignedShort()];
      for (int i = 1; i < utf.length; ++i) {
        int tag = input.readUnsignedByte();
        switch (tag) {
          case 1 -> utf[i] = input.readUTF();
          case 3, 4, 9, 10, 11, 12, 17, 18 -> input.skipNBytes(4);
          case 5, 6 -> { input.skipNBytes(8); ++i; }
          case 7, 8, 16, 19, 20 -> input.skipNBytes(2);
          case 15 -> input.skipNBytes(3);
          default -> throw new IOException("Unsupported constant pool tag " + tag);
        }
      }
      input.skipNBytes(6); // access_flags, this_class, super_class
      input.skipNBytes(2L * input.readUnsignedShort());
      int fields = input.readUnsignedShort();
      for (int i = 0; i < fields; ++i) { input.skipNBytes(6); skipAttributes(input); }
      ArrayList<String> result = new ArrayList<>();
      int methods = input.readUnsignedShort();
      for (int i = 0; i < methods; ++i) {
        int flags = input.readUnsignedShort();
        int name = input.readUnsignedShort(), descriptor = input.readUnsignedShort();
        boolean nativeMethod = (flags & 0x0100) != 0; // JVMS 4.6 ACC_NATIVE
        if (nativeMethod && (name >= utf.length || descriptor >= utf.length || utf[name] == null || utf[descriptor] == null))
          throw new IOException("Invalid native method declaration");
        boolean critical = false;
        int attributes = input.readUnsignedShort();
        for (int a = 0; a < attributes; ++a) {
          int attributeName = input.readUnsignedShort();
          long length = Integer.toUnsignedLong(input.readInt());
          String attribute = attributeName < utf.length ? utf[attributeName] : null;
          if (criticalOnly && nativeMethod && ("RuntimeInvisibleAnnotations".equals(attribute)
              || "RuntimeVisibleAnnotations".equals(attribute))) {
            byte[] body = input.readNBytes((int) length);
            critical |= annotated(new DataInputStream(new ByteArrayInputStream(body)), utf, CRITICAL);
          } else {
            input.skipNBytes(length);
          }
        }
        // ART accepts @CriticalNative only on static methods (ACC_STATIC 0x0008).
        if (nativeMethod && (!criticalOnly || (critical && (flags & 0x0008) != 0))) {
          result.add(utf[name]); result.add(utf[descriptor]);
        }
      }
      return result.toArray(String[]::new);
    } catch (IOException e) {
      throw new LinkageError("Cannot read native declarations for " + cls.getName(), e);
    }
  }
  // JVMS 4.7.16: annotations, each a type index followed by element-value pairs.
  private static boolean annotated(DataInputStream input, String[] utf, String type) throws IOException {
    boolean found = false;
    int count = input.readUnsignedShort();
    for (int i = 0; i < count; ++i) {
      int index = input.readUnsignedShort();
      found |= index < utf.length && type.equals(utf[index]);
      skipPairs(input);
    }
    return found;
  }
  private static void skipPairs(DataInputStream input) throws IOException {
    int pairs = input.readUnsignedShort();
    for (int p = 0; p < pairs; ++p) { input.skipNBytes(2); skipElement(input); }
  }
  private static void skipElement(DataInputStream input) throws IOException {
    int tag = input.readUnsignedByte();
    switch (tag) {
      case 'e' -> input.skipNBytes(4);
      case '@' -> { input.skipNBytes(2); skipPairs(input); }
      case '[' -> { int n = input.readUnsignedShort(); for (int i = 0; i < n; ++i) skipElement(input); }
      default -> input.skipNBytes(2); // const_value_index or class_info_index
    }
  }
  private static void skipAttributes(DataInputStream input) throws IOException {
    int count = input.readUnsignedShort();
    for (int i = 0; i < count; ++i) {
      input.readUnsignedShort();
      input.skipNBytes(Integer.toUnsignedLong(input.readInt()));
    }
  }
}
