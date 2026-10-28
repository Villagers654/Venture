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
    static boolean load(String owner, String name, String desc) {
        return (owner.equals("java/lang/System") || owner.equals("java/lang/Runtime"))
            && (name.equals("load") || name.equals("loadLibrary")) && desc.equals("(Ljava/lang/String;)V");
    }
    static boolean exit(String owner, String name, String desc) {
        return (owner.equals("java/lang/System") || owner.equals("java/lang/Runtime"))
            && name.equals("exit") && desc.equals("(I)V");
    }
    public static void main(String[] args) throws Exception {
        long started = System.nanoTime();
        Hierarchy hierarchy = new Hierarchy();
        for (String p : args[3].split(java.util.regex.Pattern.quote(File.pathSeparator))) hierarchy.jar(p);
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
                reader.accept(new ClassVisitor(Opcodes.ASM9, writer) {
                    @Override public MethodVisitor visitMethod(int access, String name, String desc, String signature, String[] exceptions) {
                        if ((access & Opcodes.ACC_NATIVE) != 0)
                            census.println("N\t" + owner + "\t" + name + "\t" + desc + "\t" + ((access & Opcodes.ACC_STATIC) != 0));
                        return new MethodVisitor(Opcodes.ASM9, super.visitMethod(access, name, desc, signature, exceptions)) {
                            final Map<String, Integer> pendingStreams = new HashMap<>();
                            void rewrite(String target) { census.println("R\t" + owner + "\t" + name + desc + "\t" + target); }
                            @Override public void visitTypeInsn(int opcode, String type) {
                                String stream = readStream(type);
                                if (opcode == Opcodes.NEW && stream != null) {
                                    pendingStreams.merge(type, 1, Integer::sum);
                                    super.visitTypeInsn(opcode, stream);
                                } else super.visitTypeInsn(opcode, type);
                            }
                            @Override public void visitMethodInsn(int opcode, String cls, String method, String descriptor, boolean itf) {
                                if (method.equals("<init>") && pendingStreams.getOrDefault(cls, 0) > 0) {
                                    pendingStreams.merge(cls, -1, Integer::sum);
                                    rewrite(cls + ".<init> virtual filesystem read");
                                    super.visitMethodInsn(opcode, readStream(cls), method, descriptor, false);
                                } else if (opcode == Opcodes.INVOKEVIRTUAL && cls.equals("java/io/File") && method.equals("exists") && descriptor.equals("()Z")) {
                                    // Only virtual calls: super.exists() (invokespecial) in a File subclass
                                    // must stay non-virtual, or GuestFiles.exists would recurse into it.
                                    rewrite("java/io/File.exists virtual filesystem query");
                                    super.visitMethodInsn(Opcodes.INVOKESTATIC, "com/venture/runtime/GuestFiles", "exists", "(Ljava/io/File;)Z", false);
                                } else if (cls.equals("java/lang/reflect/Method") && method.equals("invoke") && descriptor.equals("(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;")) {
                                    rewrite("reflective native-load interception");
                                    super.visitMethodInsn(Opcodes.INVOKESTATIC, LOADER, "invoke", "(Ljava/lang/reflect/Method;Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;", false);
                                } else if (load(cls, method, descriptor) || exit(cls, method, descriptor)) {
                                    // Runtime calls still evaluate and null-check their receiver.
                                    if (opcode != Opcodes.INVOKESTATIC) {
                                        super.visitInsn(Opcodes.SWAP);
                                        super.visitMethodInsn(Opcodes.INVOKESTATIC, "java/util/Objects", "requireNonNull", "(Ljava/lang/Object;)Ljava/lang/Object;", false);
                                        super.visitInsn(Opcodes.POP);
                                    }
                                    rewrite(cls + "." + method);
                                    super.visitMethodInsn(Opcodes.INVOKESTATIC, LOADER, method, descriptor, false);
                                } else super.visitMethodInsn(opcode, cls, method, descriptor, itf);
                            }
                            Object constant(Object value) {
                                if (value instanceof Handle h && (load(h.getOwner(), h.getName(), h.getDesc()) || exit(h.getOwner(), h.getName(), h.getDesc()))) {
                                    rewrite("handle:" + h.getOwner() + "." + h.getName());
                                    String d = h.getTag() == Opcodes.H_INVOKESTATIC ? h.getDesc()
                                        : "(Ljava/lang/Runtime;" + h.getDesc().substring(1);
                                    return new Handle(Opcodes.H_INVOKESTATIC, LOADER, h.getName(), d, false);
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
