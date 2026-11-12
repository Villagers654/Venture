package com.venture.dex;

import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.jar.*;
import org.objectweb.asm.*;

/** Redirects guest native loading and process exit; retains flags and descriptors. */
public final class Rewrite {
    private static final String LOADER = "com/venture/runtime/NativeLoader";
    static String readStream(String owner) {
        return switch (owner) {
            case "java/io/FileReader" -> "com/venture/runtime/GuestFileReader";
            case "java/io/FileInputStream" -> "com/venture/runtime/GuestFileInputStream";
            default -> null;
        };
    }
    static boolean hostPathConstructor(String owner, String desc) {
        return switch (owner) {
            case "java/io/File" -> desc.equals("(Ljava/lang/String;)V");
            case "java/io/RandomAccessFile" -> desc.equals("(Ljava/lang/String;Ljava/lang/String;)V");
            case "java/io/FileOutputStream", "java/io/FileWriter" ->
                desc.equals("(Ljava/lang/String;)V") || desc.equals("(Ljava/lang/String;Z)V");
            default -> false;
        };
    }
    static boolean load(String owner, String name, String desc) {
        return (owner.equals("java/lang/System") || owner.equals("java/lang/Runtime"))
            && (name.equals("load") || name.equals("loadLibrary")) && desc.equals("(Ljava/lang/String;)V");
    }
    static boolean mapLibraryName(String owner, String name, String desc) {
        return owner.equals("java/lang/System") && name.equals("mapLibraryName")
            && desc.equals("(Ljava/lang/String;)Ljava/lang/String;");
    }
    static boolean exit(String owner, String name, String desc) {
        return (owner.equals("java/lang/System") || owner.equals("java/lang/Runtime"))
            && name.equals("exit") && desc.equals("(I)V");
    }
    /** Interfaces this class super-calls (invoke-super) but implements only through an ancestor.
     *  ART resolves such a call by assignability; the JVM accepts an interface invokespecial only
     *  on a direct superinterface (JVMS 4.9.2), and R8 class merging leaves indirect ones behind.
     *  Declaring an interface the class already implements changes neither its type nor method
     *  selection, since the set of superinterfaces stays the same. */
    static List<String> inheritedSuperInterfaceCalls(ClassReader reader, Hierarchy hierarchy) {
        String self = reader.getClassName();
        Set<String> direct = new HashSet<>(Arrays.asList(reader.getInterfaces()));
        Set<String> found = new LinkedHashSet<>();
        reader.accept(new ClassVisitor(Opcodes.ASM9) {
            @Override public MethodVisitor visitMethod(int access, String name, String desc, String signature, String[] exceptions) {
                return new MethodVisitor(Opcodes.ASM9) {
                    @Override public void visitMethodInsn(int opcode, String cls, String method, String descriptor, boolean itf) {
                        if (opcode != Opcodes.INVOKESPECIAL || method.equals("<init>") || cls.equals(self)
                                || direct.contains(cls) || found.contains(cls)) return;
                        Hierarchy.Info info = hierarchy.types.get(cls);
                        if (info == null || !info.isInterface()) return;
                        try {
                            if (hierarchy.assignable(cls, self, new HashSet<>())) found.add(cls);
                        } catch (IllegalArgumentException unknownAncestry) {
                            // The frame pass reports the missing type.
                        }
                    }
                };
            }
        }, ClassReader.SKIP_DEBUG | ClassReader.SKIP_FRAMES);
        return new ArrayList<>(found);
    }
    public static void main(String[] args) throws Exception {
        long started = System.nanoTime();
        Hierarchy hierarchy = new Hierarchy();
        for (String p : args[3].split(java.util.regex.Pattern.quote(File.pathSeparator))) hierarchy.jar(p, true);
        hierarchy.jar(args[0]);
        if (args.length > 4)
            for (String p : args[4].split(java.util.regex.Pattern.quote(File.pathSeparator))) hierarchy.metadata(p);
        long loaded = System.nanoTime();
        long writing = 0;
        try (JarFile in = new JarFile(args[0]); JarOutputStream out = new JarOutputStream(new BufferedOutputStream(Files.newOutputStream(Path.of(args[1]))));
             PrintWriter census = new PrintWriter(args[2], "UTF-8")) {
            var entries = in.entries();
            while (entries.hasMoreElements()) {
                var entry = entries.nextElement();
                if (entry.isDirectory() || !entry.getName().endsWith(".class")) continue;
                byte[] original = in.getInputStream(entry).readAllBytes();
                ClassReader reader = new ClassReader(original);
                ClassWriter writer = new ClassWriter(reader, ClassWriter.COMPUTE_MAXS);
                String owner = reader.getClassName();
                census.println("C\t" + owner);
                List<String> superCalls = inheritedSuperInterfaceCalls(reader, hierarchy);
                for (String itf : superCalls) census.println("R\t" + owner + "\t<class>\tdirect superinterface " + itf);
                reader.accept(new ClassVisitor(Opcodes.ASM9, writer) {
                    @Override public void visit(int version, int access, String name, String signature, String superName, String[] interfaces) {
                        if (!superCalls.isEmpty()) {
                            List<String> all = new ArrayList<>(Arrays.asList(interfaces == null ? new String[0] : interfaces));
                            all.addAll(superCalls);
                            interfaces = all.toArray(new String[0]);
                        }
                        super.visit(version, access, name, signature, superName, interfaces);
                    }
                    @Override public MethodVisitor visitMethod(int access, String name, String desc, String signature, String[] exceptions) {
                        if ((access & Opcodes.ACC_NATIVE) != 0)
                            census.println("N\t" + owner + "\t" + name + "\t" + desc + "\t" + ((access & Opcodes.ACC_STATIC) != 0));
                        return new MethodVisitor(Opcodes.ASM9, super.visitMethod(access, name, desc, signature, exceptions)) {
                            final Map<String, Integer> pendingStreams = new HashMap<>();
                            void rewrite(String target) { census.println("R\t" + owner + "\t" + name + desc + "\t" + target); }
                            // A framework member the app references but Venture's framework lacks fails with
                            // NoSuchMethodError/NoSuchFieldError when reached; list each one once per class.
                            final Set<String> reported = new HashSet<>();
                            void member(String cls, String member, String kind) {
                                if (cls.startsWith("[") || !Boolean.FALSE.equals(hierarchy.resolves(cls, member))) return;
                                if (reported.add(cls + "." + member)) census.println("M\t" + owner + "\t" + cls + "." + member + "\t" + kind);
                            }
                            @Override public void visitFieldInsn(int opcode, String cls, String field, String descriptor) {
                                member(cls, field + " " + descriptor, "java.lang.NoSuchFieldError");
                                super.visitFieldInsn(opcode, cls, field, descriptor);
                            }
                            @Override public void visitTypeInsn(int opcode, String type) {
                                String stream = readStream(type);
                                if (opcode == Opcodes.NEW && stream != null) {
                                    pendingStreams.merge(type, 1, Integer::sum);
                                    super.visitTypeInsn(opcode, stream);
                                } else super.visitTypeInsn(opcode, type);
                            }
                            @Override public void visitMethodInsn(int opcode, String cls, String method, String descriptor, boolean itf) {
                                member(cls, method + descriptor, "java.lang.NoSuchMethodError");
                                if (method.equals("<init>") && pendingStreams.getOrDefault(cls, 0) > 0) {
                                    pendingStreams.merge(cls, -1, Integer::sum);
                                    rewrite(cls + ".<init> virtual filesystem read");
                                    super.visitMethodInsn(opcode, readStream(cls), method, descriptor, false);
                                } else if (opcode == Opcodes.INVOKEVIRTUAL && cls.equals("java/io/File") && method.equals("exists") && descriptor.equals("()Z")) {
                                    // Only virtual calls: super.exists() (invokespecial) in a File subclass
                                    // must stay non-virtual, or GuestFiles.exists would recurse into it.
                                    rewrite("java/io/File.exists virtual filesystem query");
                                    super.visitMethodInsn(Opcodes.INVOKESTATIC, "com/venture/runtime/GuestFiles", "exists", "(Ljava/io/File;)Z", false);
                                } else if (opcode == Opcodes.INVOKESPECIAL && cls.equals("java/io/File") && method.equals("<init>")
                                        && (descriptor.equals("(Ljava/io/File;Ljava/lang/String;)V") || descriptor.equals("(Ljava/lang/String;Ljava/lang/String;)V"))) {
                                    // parent, child -> parent, GuestFiles.child(parent, child)
                                    rewrite("java/io/File.<init> child path join");
                                    super.visitInsn(Opcodes.DUP2);
                                    super.visitMethodInsn(Opcodes.INVOKESTATIC, "com/venture/runtime/GuestFiles", "child", "(Ljava/lang/Object;Ljava/lang/String;)Ljava/lang/String;", false);
                                    super.visitInsn(Opcodes.SWAP);
                                    super.visitInsn(Opcodes.POP);
                                    if (descriptor.startsWith("(Ljava/lang/String;")) {
                                        super.visitInsn(Opcodes.SWAP);
                                        hostPath();
                                        super.visitInsn(Opcodes.SWAP);
                                    }
                                    super.visitMethodInsn(opcode, cls, method, descriptor, itf);
                                } else if (opcode == Opcodes.INVOKESPECIAL && method.equals("<init>") && hostPathConstructor(cls, descriptor)) {
                                    // Android absolute paths (/storage/emulated/0/..., /data/user/0/<pkg>/...) that the
                                    // app hardcodes or gets from native code name the application's host directories.
                                    rewrite(cls + ".<init> guest path");
                                    if (descriptor.equals("(Ljava/lang/String;)V")) hostPath();
                                    else {  // path, one category-1 argument (mode, append flag)
                                        super.visitInsn(Opcodes.SWAP);
                                        hostPath();
                                        super.visitInsn(Opcodes.SWAP);
                                    }
                                    super.visitMethodInsn(opcode, cls, method, descriptor, itf);
                                } else if (cls.equals("java/lang/reflect/Method") && method.equals("invoke") && descriptor.equals("(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;")) {
                                    rewrite("reflective native-load interception");
                                    super.visitMethodInsn(Opcodes.INVOKESTATIC, LOADER, "invoke", "(Ljava/lang/reflect/Method;Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;", false);
                                } else if (opcode == Opcodes.INVOKESTATIC && mapLibraryName(cls, method, descriptor)) {
                                    // Android's "lib<name>.so", not the host's "<name>.dll" (Roblox opens its
                                    // nativeLibraryDir copy by that name to prewarm it).
                                    rewrite(cls + "." + method);
                                    super.visitMethodInsn(Opcodes.INVOKESTATIC, LOADER, method, descriptor, false);
                                } else if (opcode == Opcodes.INVOKESTATIC && cls.equals("java/nio/ByteBuffer") && method.equals("allocateDirect")
                                        && descriptor.equals("(I)Ljava/nio/ByteBuffer;")) {
                                    // Native code reaches direct buffers by address; a 32-bit guest needs storage it can address.
                                    rewrite("java/nio/ByteBuffer.allocateDirect guest-addressable storage");
                                    super.visitMethodInsn(opcode, "com/venture/runtime/DirectBuffers", method, descriptor, false);
                                } else if (opcode == Opcodes.INVOKEVIRTUAL && cls.equals("java/nio/ByteBuffer")
                                        && ((method.equals("array") && descriptor.equals("()[B"))
                                            || (method.equals("arrayOffset") && descriptor.equals("()I")))) {
                                    // ART direct buffers are array-backed (Unity's UnityWebRequest writes array()).
                                    rewrite("java/nio/ByteBuffer." + method + " direct buffer contents");
                                    super.visitMethodInsn(Opcodes.INVOKESTATIC, "com/venture/runtime/DirectBuffers", method,
                                            "(Ljava/nio/ByteBuffer;)" + descriptor.substring(2), false);
                                } else if (load(cls, method, descriptor) || exit(cls, method, descriptor)) {
                                    // Runtime calls still evaluate and null-check their receiver.
                                    if (opcode != Opcodes.INVOKESTATIC) {
                                        super.visitInsn(Opcodes.SWAP);
                                        super.visitMethodInsn(Opcodes.INVOKESTATIC, "java/util/Objects", "requireNonNull", "(Ljava/lang/Object;)Ljava/lang/Object;", false);
                                        super.visitInsn(Opcodes.POP);
                                    }
                                    rewrite(cls + "." + method);
                                    super.visitMethodInsn(Opcodes.INVOKESTATIC, LOADER, method, descriptor, false);
                                } else if ((opcode == Opcodes.INVOKESTATIC || opcode == Opcodes.INVOKESPECIAL) && !itf && isInterface(cls)) {
                                    // DEX invoke-static/direct does not say whether the owner is an interface,
                                    // and dex2jar emits a Methodref. The JVM requires an InterfaceMethodref
                                    // (IncompatibleClassChangeError), e.g. D8 lambdas calling a static
                                    // lambda$ body on an interface.
                                    rewrite("interface method reference");
                                    super.visitMethodInsn(opcode, cls, method, descriptor, true);
                                } else super.visitMethodInsn(opcode, cls, method, descriptor, itf);
                            }
                            void hostPath() {
                                super.visitMethodInsn(Opcodes.INVOKESTATIC, "com/venture/runtime/GuestFiles", "host", "(Ljava/lang/String;)Ljava/lang/String;", false);
                            }
                            boolean isInterface(String type) {
                                Hierarchy.Info info = hierarchy.types.get(type);
                                return info != null && info.isInterface();
                            }
                            Object constant(Object value) {
                                if (value instanceof Handle h && (load(h.getOwner(), h.getName(), h.getDesc()) || exit(h.getOwner(), h.getName(), h.getDesc()))) {
                                    rewrite("handle:" + h.getOwner() + "." + h.getName());
                                    String d = h.getTag() == Opcodes.H_INVOKESTATIC ? h.getDesc()
                                        : "(Ljava/lang/Runtime;" + h.getDesc().substring(1);
                                    return new Handle(Opcodes.H_INVOKESTATIC, LOADER, h.getName(), d, false);
                                }
                                if (value instanceof Handle h && !h.isInterface() && isInterface(h.getOwner())
                                        && (h.getTag() == Opcodes.H_INVOKESTATIC || h.getTag() == Opcodes.H_INVOKESPECIAL)) {
                                    rewrite("interface method handle");
                                    return new Handle(h.getTag(), h.getOwner(), h.getName(), h.getDesc(), true);
                                }
                                if (value instanceof ConstantDynamic c) {
                                    Object[] a = new Object[c.getBootstrapMethodArgumentCount()];
                                    for (int i = 0; i < a.length; ++i) a[i] = constant(c.getBootstrapMethodArgument(i));
                                    return new ConstantDynamic(c.getName(), c.getDescriptor(), (Handle)constant(c.getBootstrapMethod()), a);
                                }
                                return value;
                            }
                            @Override public void visitLdcInsn(Object value) {
                                if (value instanceof String s && (s.contains("d2j fail translate") || s.contains("not supported by dex2jar")))
                                    census.println("E\t" + owner + "\t" + name + desc + "\tconverter error method");
                                super.visitLdcInsn(constant(value));
                            }
                            @Override public void visitInvokeDynamicInsn(String n, String d, Handle bsm, Object... a) {
                                for (int i = 0; i < a.length; ++i) a[i] = constant(a[i]);
                                super.visitInvokeDynamicInsn(n, d, (Handle)constant(bsm), a);
                            }
                        };
                    }
                }, 0);
                JarEntry copy = new JarEntry(entry.getName());
                copy.setTime(0);
                out.putNextEntry(copy);
                byte[] rewritten = writer.toByteArray();
                try {
                    Hierarchy.FrameResult framed = hierarchy.frames(rewritten);
                    rewritten = framed.bytes();
                    for (String failure : framed.failures()) census.println("F\t" + owner + "\t" + failure);
                }
                catch (RuntimeException e) {
                    census.println("F\t" + owner + "\t<class>\t" + e.toString().replace('\n',' '));
                }
                long start = System.nanoTime();
                out.write(rewritten);
                out.closeEntry();
                writing += System.nanoTime() - start;
            }
        }
        System.out.printf(Locale.ROOT, "phases hierarchy=%.6f link=%.6f write=%.6f%n",
            (loaded-started)/1e9, (System.nanoTime()-loaded-writing)/1e9, writing/1e9);
    }
}
