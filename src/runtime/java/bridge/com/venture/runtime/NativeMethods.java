package com.venture.runtime;

import java.io.*;
import java.util.ArrayList;

/** Reads JNI declarations without resolving unrelated Java method parameter types. */
public final class NativeMethods {
  private NativeMethods() {}
  public static String[] declarations(Class<?> cls) {
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
        if ((flags & 0x0100) != 0) { // JVMS 4.6 ACC_NATIVE
          if (name >= utf.length || descriptor >= utf.length || utf[name] == null || utf[descriptor] == null)
            throw new IOException("Invalid native method declaration");
          result.add(utf[name]); result.add(utf[descriptor]);
        }
        skipAttributes(input);
      }
      return result.toArray(String[]::new);
    } catch (IOException e) {
      throw new LinkageError("Cannot read native declarations for " + cls.getName(), e);
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
