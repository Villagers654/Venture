// Venture: prepares layoutlib.jar for Venture's hosted framework (desktop only).
//
// Usage: java -cp asm.jar ProcessLayoutlib.java <layoutlib.jar or its extracted
//        directory> <out.jar> <delegates.txt> <rules.txt>...
//
// Each rules file (src/runtime/layoutlib/layoutlib-rules.txt, and the classes
// tools/layoutlib/select_sources.py takes from Venture) holds one rule per line:
//   exclude <class or package.**>  Venture supplies the class (or the package), so
//                                   layoutlib's copy is left out of the output.
//                                   Its anonymous and local classes go with it;
//                                   its member classes stay for framework code
//                                   that names one Venture's class lacks; once
//                                   Venture's jar is compiled, --prune leaves
//                                   out the ones it has.
//   native <class>                 layoutlib's build turned the class's native
//                                   methods into stubs that return 0 or null
//                                   (com.android.tools.layoutlib.create
//                                   .OverrideMethod) or into calls to a delegate;
//                                   its natives ship in libandroid_runtime all the
//                                   same. Each such method becomes a native
//                                   method again, so that library registers the
//                                   real implementation.
//   undelegate <class or package.**>
//                                  layoutlib's build moved some methods' AOSP
//                                  bodies to <name>_Original and made <name> call
//                                  a <Class>_Delegate written for Android
//                                  Studio's preview (frozen time, resources from
//                                  the IDE, edit mode). Each such method gets its
//                                  AOSP body back under its own name.
//   keep-delegate <class>.<method> a method the undelegate rule leaves alone.
//   delegate-natives <class>       the class's natives, which layoutlib's build
//                                  turned into OverrideMethod stubs, call a static
//                                  method of the same name in <class>_Delegate
//                                  instead (the instance first for an instance
//                                  method), which Venture's src/runtime/layoutlib
//                                  java implements (MessageQueue's looper).
//   delegate-native <class>.<method>
//                                  the same for one method's stubs.
//   ravenwood <class>.<method>     the method runs its Ravenwood version
//                                  (<method>$ravenwood, AOSP's host JVM runtime).
//   forward <class>.<method>:<other>
//                                  an instance method layoutlib added for the
//                                  preview returns this.<other>(same arguments),
//                                  the call AOSP makes in its place.
//   window-handle <class>          the class (android.view.Surface) also carries
//                                  a Venture native window: a long field
//                                  ventureWindow, ventureNativeWindow() and a
//                                  static ventureAdopt(long), which Venture's
//                                  natives and host services use as on the
//                                  classic framework's Surface.
// A line's text after '#' is a comment.
//
// delegates.txt lists every call into a *_Delegate class left in the output:
// Venture's own delegates (src/runtime/layoutlib/java) implement those.
import java.io.FileOutputStream;
import java.io.PrintWriter;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeSet;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipOutputStream;
import org.objectweb.asm.AnnotationVisitor;
import org.objectweb.asm.ClassReader;
import org.objectweb.asm.ClassVisitor;
import org.objectweb.asm.ClassWriter;
import org.objectweb.asm.MethodVisitor;
import org.objectweb.asm.Opcodes;

public class ProcessLayoutlib {
  static final String ORIGINAL = "_Original";

  public static void main(String[] args) throws Exception {
    if (args.length == 4 && args[0].equals("--prune")) {
      prune(Path.of(args[1]), Path.of(args[2]), Path.of(args[3]));
      return;
    }
    if (args.length < 4) throw new IllegalArgumentException("layoutlib.jar out.jar delegates.txt rules.txt...");
    List<String> excluded = new ArrayList<>(), undelegated = new ArrayList<>();
    Set<String> nativized = new HashSet<>(), kept = new HashSet<>(), windowHandles = new HashSet<>(),
        delegatedNatives = new HashSet<>();
    // delegate-native and ravenwood: class -> method names
    Map<String, Set<String>> delegatedMethods = new HashMap<>(), ravenwood = new HashMap<>();
    // forward: class -> method name -> the method it returns instead
    Map<String, Map<String, String>> forwards = new HashMap<>();
    List<String> lines = new ArrayList<>();
    for (int i = 3; i < args.length; ++i) lines.addAll(Files.readAllLines(Path.of(args[i])));
    for (String raw : lines) {
      String line = raw.replaceAll("#.*", "").trim();
      if (line.isEmpty()) continue;
      String[] parts = line.split("\\s+");
      if (parts.length != 2) throw new IllegalArgumentException("bad rule: " + raw);
      String name = parts[1].replace('.', '/');
      switch (parts[0]) {
        case "exclude" -> excluded.add(name);
        case "native" -> nativized.add(name);
        case "undelegate" -> undelegated.add(name);
        case "keep-delegate" -> kept.add(name);
        case "window-handle" -> windowHandles.add(name);
        case "delegate-natives" -> delegatedNatives.add(name);
        case "delegate-native" -> delegatedMethods.computeIfAbsent(name.substring(0, name.lastIndexOf('/')), k -> new HashSet<>())
            .add(name.substring(name.lastIndexOf('/') + 1));
        case "ravenwood" -> ravenwood.computeIfAbsent(name.substring(0, name.lastIndexOf('/')), k -> new HashSet<>())
            .add(name.substring(name.lastIndexOf('/') + 1));
        case "forward" -> {
          int colon = name.indexOf(':');
          if (colon < 0) throw new IllegalArgumentException("bad rule: " + raw);
          String method = name.substring(0, colon);
          forwards.computeIfAbsent(method.substring(0, method.lastIndexOf('/')), k -> new HashMap<>())
              .put(method.substring(method.lastIndexOf('/') + 1), name.substring(colon + 1));
        }
        default -> throw new IllegalArgumentException("bad rule: " + raw);
      }
    }
    int dropped = 0, opened = 0;
    int[] restored = {0}, original = {0};
    Set<String> undelegatedMethods = new TreeSet<>();
    Set<String> seenNative = new HashSet<>();
    Set<String> delegateCalls = new TreeSet<>();
    Path temporary = Path.of(args[1] + ".tmp");
    try (Input in = Input.open(Path.of(args[0]));
         ZipOutputStream out = new ZipOutputStream(new FileOutputStream(temporary.toFile()))) {
      for (String name : in.names()) {
        if (name.startsWith("META-INF/")) continue;
        byte[] data = in.read(name);
        if (name.endsWith(".class")) {
          String cls = name.substring(0, name.length() - 6);
          if (matches(excluded, enclosingNamed(cls))) { ++dropped; continue; }
          boolean nat = nativized.contains(cls);
          if (nat) seenNative.add(cls);
          data = transform(data, cls, nat, matches(undelegated, cls), kept, restored, original, undelegatedMethods);
          String host = nestHost(data);
          if (host != null && matches(excluded, host)) { data = openNest(data); ++opened; }
          if (windowHandles.remove(cls)) data = addWindowHandle(data, cls);
          if (delegatedNatives.remove(cls)) data = delegateNatives(data, cls, null);
          Set<String> methods = delegatedMethods.remove(cls);
          if (methods != null) data = delegateNatives(data, cls, methods);
          methods = ravenwood.remove(cls);
          if (methods != null) data = useRavenwood(data, cls, methods);
          Map<String, String> forwarded = forwards.remove(cls);
          if (forwarded != null) data = forward(data, cls, forwarded);
          collectDelegateCalls(data, cls, delegateCalls);
        }
        ZipEntry copy = new ZipEntry(name);
        copy.setTime(FIXED_TIME);
        out.putNextEntry(copy);
        out.write(data);
        out.closeEntry();
      }
    }
    if (!delegatedMethods.isEmpty()) throw new IllegalStateException("delegate-native rule names a missing class: " + delegatedMethods.keySet());
    if (!ravenwood.isEmpty()) throw new IllegalStateException("ravenwood rule names a missing class: " + ravenwood.keySet());
    if (!forwards.isEmpty()) throw new IllegalStateException("forward rule names a missing class: " + forwards.keySet());
    if (!delegatedNatives.isEmpty()) throw new IllegalStateException("delegate-natives rule names a missing class: " + delegatedNatives);
    if (!windowHandles.isEmpty()) throw new IllegalStateException("window-handle rule names a missing class: " + windowHandles);
    for (String cls : nativized)
      if (!seenNative.contains(cls)) throw new IllegalStateException("native rule names a missing class: " + cls);
    Files.move(temporary, Path.of(args[1]), StandardCopyOption.REPLACE_EXISTING);
    try (PrintWriter writer = new PrintWriter(args[2], "UTF-8")) {
      writer.println("# Calls into *_Delegate classes left in the processed layoutlib.jar (tools/layoutlib/ProcessLayoutlib.java)");
      delegateCalls.forEach(writer::println);
      writer.println("# AOSP bodies restored (undelegate)");
      undelegatedMethods.forEach(m -> writer.println("# " + m));
    }
    System.out.printf("layoutlib: %d classes left out, %d member classes opened, %d native methods restored, %d AOSP bodies restored, %d delegate calls left%n",
        dropped, opened, restored[0], original[0], delegateCalls.size());
  }

  // The class's nest host (NestHost attribute), or null for a class that is its own.
  static String nestHost(byte[] data) {
    String[] host = {null};
    new ClassReader(data).accept(new ClassVisitor(Opcodes.ASM9) {
      @Override
      public void visitNestHost(String nest) { host[0] = nest; }
    }, ClassReader.SKIP_CODE | ClassReader.SKIP_DEBUG | ClassReader.SKIP_FRAMES);
    return host[0];
  }

  // A member class whose outer class Venture supplies has lost its nest: Venture's
  // outer class does not name it as a nest member, so the JVM refuses the private
  // access nestmates share (Settings$Config constructing Settings$NameValueCache).
  // The class leaves the nest and its private members become package-private, the
  // access javac gave nestmates before Java 11 (an interface's become public).
  static byte[] openNest(byte[] data) {
    ClassReader reader = new ClassReader(data);
    ClassWriter writer = new ClassWriter(reader, 0);
    reader.accept(new ClassVisitor(Opcodes.ASM9, writer) {
      boolean isInterface;
      @Override
      public void visit(int version, int access, String name, String signature, String superName, String[] interfaces) {
        isInterface = (access & Opcodes.ACC_INTERFACE) != 0;
        super.visit(version, access, name, signature, superName, interfaces);
      }
      @Override
      public void visitNestHost(String nestHost) {}
      @Override
      public void visitNestMember(String nestMember) {}
      @Override
      public org.objectweb.asm.FieldVisitor visitField(int access, String name, String descriptor, String signature, Object value) {
        return super.visitField(access & ~Opcodes.ACC_PRIVATE, name, descriptor, signature, value);
      }
      @Override
      public MethodVisitor visitMethod(int access, String name, String descriptor, String signature, String[] exceptions) {
        if ((access & Opcodes.ACC_PRIVATE) != 0)
          access = (access & ~Opcodes.ACC_PRIVATE) | (isInterface ? Opcodes.ACC_PUBLIC : 0);
        return super.visitMethod(access, name, descriptor, signature, exceptions);
      }
    }, 0);
    return writer.toByteArray();
  }

  // --prune <processed.jar> <venture-framework-2d.jar> <out.jar>: leaves out the
  // classes Venture's jar has (member classes of the classes it replaces), with
  // their anonymous and local classes, once that jar is compiled.
  static void prune(Path input, Path venture, Path output) throws Exception {
    Set<String> shadowed = new HashSet<>();
    try (Input classes = Input.open(venture)) {
      for (String name : classes.names())
        if (name.endsWith(".class")) shadowed.add(name.substring(0, name.length() - 6));
    }
    int dropped = 0;
    Path temporary = Path.of(output + ".tmp");
    try (Input in = Input.open(input);
         ZipOutputStream out = new ZipOutputStream(new FileOutputStream(temporary.toFile()))) {
      for (String name : in.names()) {
        if (name.endsWith(".class") && shadowed.contains(enclosingNamed(name.substring(0, name.length() - 6)))) {
          ++dropped;
          continue;
        }
        ZipEntry copy = new ZipEntry(name);
        copy.setTime(FIXED_TIME);
        out.putNextEntry(copy);
        out.write(in.read(name));
        out.closeEntry();
      }
    }
    Files.move(temporary, output, StandardCopyOption.REPLACE_EXISTING);
    System.out.printf("layoutlib: %d member classes Venture supplies left out%n", dropped);
  }

  // 1980-01-01 local time, so the output depends only on its input.
  static final long FIXED_TIME = 315532800000L;

  /** The input layoutlib.jar, or the directory it was extracted into. */
  interface Input extends AutoCloseable {
    List<String> names() throws java.io.IOException;
    byte[] read(String name) throws java.io.IOException;
    @Override void close() throws java.io.IOException;
    static Input open(Path path) throws java.io.IOException {
      if (Files.isDirectory(path)) {
        return new Input() {
          public List<String> names() throws java.io.IOException {
            try (var files = Files.walk(path)) {
              return files.filter(Files::isRegularFile)
                  .map(file -> path.relativize(file).toString().replace(java.io.File.separatorChar, '/')).sorted().toList();
            }
          }
          public byte[] read(String name) throws java.io.IOException { return Files.readAllBytes(path.resolve(name)); }
          public void close() {}
        };
      }
      ZipFile zip = new ZipFile(path.toFile());
      return new Input() {
        public List<String> names() {
          List<String> names = new ArrayList<>();
          for (var entries = zip.entries(); entries.hasMoreElements();) {
            ZipEntry entry = entries.nextElement();
            if (!entry.isDirectory()) names.add(entry.getName());
          }
          return names;
        }
        public byte[] read(String name) throws java.io.IOException {
          return zip.getInputStream(zip.getEntry(name)).readAllBytes();
        }
        public void close() throws java.io.IOException { zip.close(); }
      };
    }
  }

  // The named class an anonymous or local class belongs to (Outer for Outer$1 and
  // Outer$1Local, Outer$Member for Outer$Member$2); a named class itself.
  static String enclosingNamed(String cls) {
    for (;;) {
      int split = cls.lastIndexOf('$');
      if (split < 0 || split + 1 == cls.length() || !Character.isDigit(cls.charAt(split + 1))) return cls;
      cls = cls.substring(0, split);
    }
  }

  static boolean matches(List<String> rules, String cls) {
    for (String rule : rules) {
      if (rule.endsWith("/**") ? cls.startsWith(rule.substring(0, rule.length() - 2)) : cls.equals(rule)) return true;
    }
    return false;
  }

  static byte[] transform(byte[] data, String cls, boolean nativize, boolean undelegate, Set<String> kept,
                          int[] restored, int[] original, Set<String> undelegatedMethods) {
    if (!nativize && !undelegate) return data;
    ClassReader reader = new ClassReader(data);
    // name+descriptor -> access of the delegating method an _Original replaces
    Map<String, Integer> replaced = new HashMap<>();
    Set<String> methods = new HashSet<>();
    reader.accept(new ClassVisitor(Opcodes.ASM9) {
      @Override
      public MethodVisitor visitMethod(int access, String name, String descriptor, String signature, String[] exceptions) {
        methods.add(name + descriptor);
        return null;
      }
    }, ClassReader.SKIP_CODE);
    if (undelegate) {
      reader.accept(new ClassVisitor(Opcodes.ASM9) {
        @Override
        public MethodVisitor visitMethod(int access, String name, String descriptor, String signature, String[] exceptions) {
          if (methods.contains(name + ORIGINAL + descriptor) && !kept.contains(cls + "/" + name)) replaced.put(name + descriptor, access);
          return null;
        }
      }, ClassReader.SKIP_CODE);
    }
    if (replaced.isEmpty() && !nativize) return data;
    // The class's former natives: stubs calling OverrideMethod, or calls into the
    // class's delegate that have no AOSP body to go back to.
    Set<String> natives = new HashSet<>();
    if (nativize) {
      reader.accept(new ClassVisitor(Opcodes.ASM9) {
        @Override
        public MethodVisitor visitMethod(int access, String name, String descriptor, String signature, String[] exceptions) {
          if (methods.contains(name + ORIGINAL + descriptor) || name.endsWith(ORIGINAL)) return null;
          return new MethodVisitor(Opcodes.ASM9) {
            @Override
            public void visitMethodInsn(int opcode, String owner, String method, String desc, boolean itf) {
              if (owner.equals("com/android/tools/layoutlib/create/OverrideMethod") || owner.equals(cls + "_Delegate"))
                natives.add(name + descriptor);
            }
          };
        }
      }, 0);
    }
    ClassWriter writer = new ClassWriter(0);
    reader.accept(new ClassVisitor(Opcodes.ASM9, writer) {
      @Override
      public MethodVisitor visitMethod(int access, String name, String descriptor, String signature, String[] exceptions) {
        if (replaced.containsKey(name + descriptor)) return null;  // the delegating stub
        if (name.endsWith(ORIGINAL)) {
          String base = name.substring(0, name.length() - ORIGINAL.length());
          Integer stubAccess = replaced.get(base + descriptor);
          if (stubAccess != null) {
            ++original[0];
            undelegatedMethods.add(cls.replace('/', '.') + "." + base);
            int visibility = Opcodes.ACC_PUBLIC | Opcodes.ACC_PROTECTED | Opcodes.ACC_PRIVATE;
            return super.visitMethod((access & ~visibility) | (stubAccess & visibility), base, descriptor, signature, exceptions);
          }
        }
        if (!natives.contains(name + descriptor))
          return super.visitMethod(access, name, descriptor, signature, exceptions);
        ++restored[0];
        // A native method has no code: annotations stay, the body is dropped.
        MethodVisitor target = super.visitMethod(access | Opcodes.ACC_NATIVE, name, descriptor, signature, exceptions);
        return new MethodVisitor(Opcodes.ASM9, null) {
          @Override
          public AnnotationVisitor visitAnnotation(String desc, boolean visible) {
            return target.visitAnnotation(desc, visible);
          }
          @Override
          public void visitEnd() { target.visitEnd(); }
        };
      }
    }, 0);
    return writer.toByteArray();
  }

  static byte[] delegateNatives(byte[] data, String cls, Set<String> only) {
    ClassReader reader = new ClassReader(data);
    Set<String> stubs = new HashSet<>();
    reader.accept(new ClassVisitor(Opcodes.ASM9) {
      @Override
      public MethodVisitor visitMethod(int access, String name, String descriptor, String signature, String[] exceptions) {
        return new MethodVisitor(Opcodes.ASM9) {
          @Override
          public void visitMethodInsn(int opcode, String owner, String method, String desc, boolean itf) {
            if (owner.equals("com/android/tools/layoutlib/create/OverrideMethod") && (only == null || only.contains(name)))
              stubs.add(name + descriptor);
          }
        };
      }
    }, 0);
    if (stubs.isEmpty()) throw new IllegalStateException("delegate-natives: no native stubs in " + cls);
    if (only != null) for (String method : only)
      if (stubs.stream().noneMatch(stub -> stub.startsWith(method + "(")))
        throw new IllegalStateException("delegate-native: no stub " + cls + "." + method);
    ClassWriter writer = new ClassWriter(reader, ClassWriter.COMPUTE_MAXS);
    reader.accept(new ClassVisitor(Opcodes.ASM9, writer) {
      @Override
      public MethodVisitor visitMethod(int access, String name, String descriptor, String signature, String[] exceptions) {
        MethodVisitor target = super.visitMethod(access, name, descriptor, signature, exceptions);
        if (!stubs.contains(name + descriptor)) return target;
        boolean isStatic = (access & Opcodes.ACC_STATIC) != 0;
        org.objectweb.asm.Type type = org.objectweb.asm.Type.getMethodType(descriptor);
        String delegateDescriptor = isStatic ? descriptor : "(L" + cls + ";" + descriptor.substring(1);
        target.visitCode();
        int slot = 0;
        if (!isStatic) target.visitVarInsn(Opcodes.ALOAD, slot++);
        for (org.objectweb.asm.Type argument : type.getArgumentTypes()) {
          target.visitVarInsn(argument.getOpcode(Opcodes.ILOAD), slot);
          slot += argument.getSize();
        }
        target.visitMethodInsn(Opcodes.INVOKESTATIC, cls + "_Delegate", name, delegateDescriptor, false);
        target.visitInsn(type.getReturnType().getOpcode(Opcodes.IRETURN));
        target.visitMaxs(0, 0);
        target.visitEnd();
        return null;  // the stub's own code is dropped
      }
    }, 0);
    return writer.toByteArray();
  }

  // ravenwood <class>.<method>: AOSP's host-JVM test runtime (Ravenwood) has its
  // own versions of some methods, named <method>$ravenwood, for a JVM where
  // Java arrays have no stable native address. Each overload of the method gets
  // a body that calls its Ravenwood version.
  static byte[] useRavenwood(byte[] data, String cls, Set<String> methods) {
    ClassReader reader = new ClassReader(data);
    Set<String> versions = new HashSet<>();
    reader.accept(new ClassVisitor(Opcodes.ASM9) {
      @Override
      public MethodVisitor visitMethod(int access, String name, String descriptor, String signature, String[] exceptions) {
        if (name.endsWith("$ravenwood")) versions.add(name.substring(0, name.length() - 10) + descriptor);
        return null;
      }
    }, 0);
    Set<String> unused = new HashSet<>(methods);
    ClassWriter writer = new ClassWriter(reader, ClassWriter.COMPUTE_MAXS);
    reader.accept(new ClassVisitor(Opcodes.ASM9, writer) {
      @Override
      public MethodVisitor visitMethod(int access, String name, String descriptor, String signature, String[] exceptions) {
        MethodVisitor target = super.visitMethod(access, name, descriptor, signature, exceptions);
        if (!methods.contains(name) || !versions.contains(name + descriptor)) return target;
        unused.remove(name);
        boolean isStatic = (access & Opcodes.ACC_STATIC) != 0;
        org.objectweb.asm.Type type = org.objectweb.asm.Type.getMethodType(descriptor);
        target.visitCode();
        int slot = 0;
        if (!isStatic) target.visitVarInsn(Opcodes.ALOAD, slot++);
        for (org.objectweb.asm.Type argument : type.getArgumentTypes()) {
          target.visitVarInsn(argument.getOpcode(Opcodes.ILOAD), slot);
          slot += argument.getSize();
        }
        target.visitMethodInsn(isStatic ? Opcodes.INVOKESTATIC : Opcodes.INVOKESPECIAL, cls, name + "$ravenwood",
            descriptor, false);
        target.visitInsn(type.getReturnType().getOpcode(Opcodes.IRETURN));
        target.visitMaxs(0, 0);
        target.visitEnd();
        return null;
      }
    }, 0);
    if (!unused.isEmpty()) throw new IllegalStateException("ravenwood: no Ravenwood version of " + cls + "." + unused);
    return writer.toByteArray();
  }

  static byte[] forward(byte[] data, String cls, Map<String, String> forwards) {
    ClassReader reader = new ClassReader(data);
    Set<String> unused = new HashSet<>(forwards.keySet());
    ClassWriter writer = new ClassWriter(reader, ClassWriter.COMPUTE_MAXS);
    reader.accept(new ClassVisitor(Opcodes.ASM9, writer) {
      @Override
      public MethodVisitor visitMethod(int access, String name, String descriptor, String signature, String[] exceptions) {
        MethodVisitor target = super.visitMethod(access, name, descriptor, signature, exceptions);
        String other = forwards.get(name);
        if (other == null || (access & (Opcodes.ACC_STATIC | Opcodes.ACC_ABSTRACT)) != 0) return target;
        unused.remove(name);
        org.objectweb.asm.Type type = org.objectweb.asm.Type.getMethodType(descriptor);
        target.visitCode();
        target.visitVarInsn(Opcodes.ALOAD, 0);
        int slot = 1;
        for (org.objectweb.asm.Type argument : type.getArgumentTypes()) {
          target.visitVarInsn(argument.getOpcode(Opcodes.ILOAD), slot);
          slot += argument.getSize();
        }
        target.visitMethodInsn(Opcodes.INVOKEVIRTUAL, cls, other, descriptor, false);
        target.visitInsn(type.getReturnType().getOpcode(Opcodes.IRETURN));
        target.visitMaxs(0, 0);
        target.visitEnd();
        return null;
      }
    }, 0);
    if (!unused.isEmpty()) throw new IllegalStateException("forward: no instance method " + cls + "." + unused);
    return writer.toByteArray();
  }

  static byte[] addWindowHandle(byte[] data, String cls) {
    ClassReader reader = new ClassReader(data);
    ClassWriter writer = new ClassWriter(reader, ClassWriter.COMPUTE_MAXS);
    reader.accept(new ClassVisitor(Opcodes.ASM9, writer) {
      // A Surface holding a Venture window is valid while it holds it, and
      // release() gives the window back (com.venture.runtime.layoutlib.Layoutlib.releaseWindow).
      // AOSP's body runs after, for a Surface without one.
      @Override
      public MethodVisitor visitMethod(int access, String name, String descriptor, String signature, String[] exceptions) {
        MethodVisitor mv = super.visitMethod(access, name, descriptor, signature, exceptions);
        boolean valid = name.equals("isValid") && descriptor.equals("()Z");
        boolean release = name.equals("release") && descriptor.equals("()V");
        if (!valid && !release) return mv;
        return new MethodVisitor(Opcodes.ASM9, mv) {
          @Override
          public void visitCode() {
            super.visitCode();
            org.objectweb.asm.Label aosp = new org.objectweb.asm.Label();
            super.visitVarInsn(Opcodes.ALOAD, 0);
            super.visitFieldInsn(Opcodes.GETFIELD, cls, "ventureWindow", "J");
            super.visitInsn(Opcodes.LCONST_0);
            super.visitInsn(Opcodes.LCMP);
            super.visitJumpInsn(Opcodes.IFEQ, aosp);
            if (valid) {
              super.visitInsn(Opcodes.ICONST_1);
              super.visitInsn(Opcodes.IRETURN);
            } else {
              super.visitVarInsn(Opcodes.ALOAD, 0);
              super.visitFieldInsn(Opcodes.GETFIELD, cls, "ventureWindow", "J");
              super.visitMethodInsn(Opcodes.INVOKESTATIC, "com/venture/runtime/layoutlib/Layoutlib", "releaseWindow", "(J)V", false);
              super.visitVarInsn(Opcodes.ALOAD, 0);
              super.visitInsn(Opcodes.LCONST_0);
              super.visitFieldInsn(Opcodes.PUTFIELD, cls, "ventureWindow", "J");
            }
            super.visitLabel(aosp);
            super.visitFrame(Opcodes.F_SAME, 0, null, 0, null);
            // Keeps a frame the method starts with at its own offset.
            super.visitInsn(Opcodes.NOP);
          }
        };
      }

      @Override
      public void visitEnd() {
        visitField(Opcodes.ACC_PRIVATE, "ventureWindow", "J", null, null).visitEnd();
        MethodVisitor get = visitMethod(Opcodes.ACC_PUBLIC | Opcodes.ACC_SYNCHRONIZED, "ventureNativeWindow", "()J", null, null);
        get.visitCode();
        get.visitVarInsn(Opcodes.ALOAD, 0);
        get.visitFieldInsn(Opcodes.GETFIELD, cls, "ventureWindow", "J");
        get.visitInsn(Opcodes.LRETURN);
        get.visitMaxs(0, 0);
        get.visitEnd();
        MethodVisitor adopt = visitMethod(Opcodes.ACC_PUBLIC | Opcodes.ACC_STATIC, "ventureAdopt", "(J)L" + cls + ";", null, null);
        adopt.visitCode();
        adopt.visitTypeInsn(Opcodes.NEW, cls);
        adopt.visitInsn(Opcodes.DUP);
        adopt.visitMethodInsn(Opcodes.INVOKESPECIAL, cls, "<init>", "()V", false);
        adopt.visitInsn(Opcodes.DUP);
        adopt.visitVarInsn(Opcodes.LLOAD, 0);
        adopt.visitFieldInsn(Opcodes.PUTFIELD, cls, "ventureWindow", "J");
        adopt.visitInsn(Opcodes.ARETURN);
        adopt.visitMaxs(0, 0);
        adopt.visitEnd();
        super.visitEnd();
      }
    }, 0);
    return writer.toByteArray();
  }

  static void collectDelegateCalls(byte[] data, String cls, Set<String> calls) {
    if (cls.endsWith("_Delegate") || cls.contains("_Delegate$")) return;
    new ClassReader(data).accept(new ClassVisitor(Opcodes.ASM9) {
      @Override
      public MethodVisitor visitMethod(int access, String name, String descriptor, String signature, String[] exceptions) {
        return new MethodVisitor(Opcodes.ASM9) {
          @Override
          public void visitMethodInsn(int opcode, String owner, String method, String desc, boolean itf) {
            if (owner.endsWith("_Delegate")) calls.add(owner.replace('/', '.') + "." + method + desc);
          }
        };
      }
    }, 0);
  }
}
