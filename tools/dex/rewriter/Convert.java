package com.venture.dex;

import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.jar.*;
import com.googlecode.d2j.Method;
import com.googlecode.d2j.dex.*;
import com.googlecode.d2j.node.*;
import com.googlecode.d2j.reader.DexFileReader;
import org.objectweb.asm.*;

/** Pinned dex2jar 2.4 API, with an output visitor suitable for execution.
 * The stock command uses LambadaNameSafeClassAdapter, renaming '-' to '_'.
 * '-' is a legal JVM unqualified-name character (JVMS 4.2.2); keep DEX names.
 */
public final class Convert {
    public static void main(String[] args) throws Exception {
        if (args.length == 0 || args.length % 3 != 0) throw new IllegalArgumentException("expected DEX/JAR/errors triples");
        // Multidex shares one JVM and its warmed translator, without keeping
        // all DEX ASTs alive together or racing dex2jar's shared transformers.
        for (int i = 0; i < args.length; i += 3) convert(Arrays.copyOfRange(args, i, i + 3));
    }
    private static void convert(String[] args) throws Exception {
        long started = System.nanoTime();
        DexFileNode dex = new DexFileNode();
        new DexFileReader(Path.of(args[0])).accept(dex, DexFileReader.KEEP_ALL_METHODS);
        long parsed = System.nanoTime();
        long[] writing = {0};
        Map<String,Integer> flags = new HashMap<>();
        for (DexClassNode cls : dex.clzs) flags.put(cls.className.substring(1, cls.className.length()-1), cls.access);
        try (JarOutputStream jar = new JarOutputStream(new BufferedOutputStream(Files.newOutputStream(Path.of(args[1]))));
             PrintWriter errors = new PrintWriter(args[2], "UTF-8")) {
            var handler = new BaseDexExceptionHandler() {
                @Override public void handleFileException(Exception e) {
                    throw new IllegalStateException("dex2jar file translation failed", e);
                }
                @Override public void handleMethodTranslateException(Method m, DexMethodNode node, MethodVisitor visitor, Exception e) {
                    errors.println(m + "\t" + e.toString().replace('\n', ' '));
                    // Preserve diagnostic output only; the Python gate rejects every such method.
                    super.handleMethodTranslateException(m, node, visitor, e);
                }
            };
            new ExDex2Asm(handler).convertDex(dex, name -> {
                ClassWriter writer = new ClassWriter(ClassWriter.COMPUTE_MAXS);
                return new ClassVisitor(Opcodes.ASM9, writer) {
                    String owner;
                    @Override public void visit(int version, int access, String n, String sig, String parent, String[] interfaces) {
                        owner = n;
                        // dex2jar merges dalvik.annotation.InnerClass flags into
                        // the class header. R8's stale annotations in Bait/Gorilla
                        // make abstract classes final/static. DEX class_def flags
                        // are authoritative; JVM header permits no static/private.
                        int original = flags.getOrDefault(n, access);
                        int clean = original & (Opcodes.ACC_PUBLIC | Opcodes.ACC_FINAL | Opcodes.ACC_INTERFACE |
                            Opcodes.ACC_ABSTRACT | Opcodes.ACC_SYNTHETIC | Opcodes.ACC_ANNOTATION | Opcodes.ACC_ENUM);
                        if ((clean & Opcodes.ACC_INTERFACE) == 0) clean |= Opcodes.ACC_SUPER;
                        super.visit(version, clean, n, sig, parent, interfaces);
                    }
                    @Override public void visitEnd() {
                        super.visitEnd();
                        try {
                            long start = System.nanoTime();
                            JarEntry entry = new JarEntry(owner + ".class"); entry.setTime(0);
                            jar.putNextEntry(entry); jar.write(writer.toByteArray()); jar.closeEntry();
                            writing[0] += System.nanoTime() - start;
                        } catch (IOException e) { throw new UncheckedIOException(e); }
                    }
                    @Override public void visitInnerClass(String n, String outer, String inner, int access) {
                        Integer original = flags.get(n);
                        if (original != null) {
                            // The same stale annotation also contaminates InnerClasses.
                            // Preserve nesting visibility/staticness, but take kind,
                            // final/abstract and synthetic flags from the real class_def.
                            access = (access & (Opcodes.ACC_PUBLIC | Opcodes.ACC_PRIVATE | Opcodes.ACC_PROTECTED | Opcodes.ACC_STATIC))
                                | (original & (Opcodes.ACC_FINAL | Opcodes.ACC_INTERFACE | Opcodes.ACC_ABSTRACT | Opcodes.ACC_SYNTHETIC | Opcodes.ACC_ANNOTATION | Opcodes.ACC_ENUM));
                        }
                        super.visitInnerClass(n, outer, inner, access);
                    }
                };
            });
        }
        System.out.printf(Locale.ROOT, "phases parse=%.6f translate=%.6f write=%.6f%n",
            (parsed-started)/1e9, (System.nanoTime()-parsed-writing[0])/1e9, writing[0]/1e9);
    }
}
