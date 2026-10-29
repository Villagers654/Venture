package com.venture.dex;

import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.jar.*;
import org.objectweb.asm.*;

/** Computes stack-map frames from class metadata only; never runs an initializer.
 * Unknown Android ancestry is an explicit blocker, never guessed as Object.
 */
final class Hierarchy {
    record Info(String parent, List<String> interfaces, boolean isInterface) {}
    final Map<String,Info> types = new HashMap<>();
    void add(byte[] bytes) {
        ClassReader r = new ClassReader(bytes);
        types.putIfAbsent(r.getClassName(), new Info(r.getSuperName(), Arrays.asList(r.getInterfaces()), (r.getAccess() & Opcodes.ACC_INTERFACE) != 0));
    }
    void jar(String path) throws IOException {
        if (path.isEmpty()) return;
        try (JarFile jar = new JarFile(path)) {
            var entries = jar.entries();
            while (entries.hasMoreElements()) {
                var e = entries.nextElement();
                if (e.getName().endsWith(".class") && !e.getName().startsWith("META-INF/")) add(jar.getInputStream(e).readAllBytes());
            }
        }
    }
    void metadata(String path) throws IOException {
        if (path.isEmpty()) return;
        for (String line : Files.readAllLines(Path.of(path))) {
            if (line.isBlank() || line.startsWith("#")) continue;
            String[] row = line.split("\t", -1);
            if (row.length != 4 || !(row[2].equals("0") || row[2].equals("1")))
                throw new IOException("Invalid type ancestry row");
            types.putIfAbsent(row[0], new Info(row[1].isEmpty() ? null : row[1],
                row[3].isEmpty() ? List.of() : List.of(row[3].split(",")), row[2].equals("1")));
        }
    }
    Info info(String name) {
        Info known = types.get(name);
        if (known != null) return known;
        try (InputStream in = ClassLoader.getPlatformClassLoader().getResourceAsStream(name + ".class")) {
            if (in == null) throw new IllegalArgumentException("missing frame hierarchy: " + name);
            add(in.readAllBytes()); return types.get(name);
        } catch (IOException e) { throw new UncheckedIOException(e); }
    }
    boolean assignable(String target, String source, Set<String> seen) {
        if (target.equals(source) || target.equals("java/lang/Object")) return true;
        if (!seen.add(source)) return false;
        if (source.startsWith("[")) {
            if (target.equals("java/lang/Cloneable") || target.equals("java/io/Serializable")) return true;
            if (!target.startsWith("[")) return false;
            String t = component(target), s = component(source);
            return t != null && s != null && assignable(t, s, new HashSet<>());
        }
        if (target.startsWith("[")) return false;
        Info i = info(source);
        for (String itf : i.interfaces()) if (assignable(target, itf, seen)) return true;
        return i.parent() != null && assignable(target, i.parent(), seen);
    }
    String component(String name) {
        String s = name.substring(1);
        return s.startsWith("[") ? s : s.startsWith("L") ? s.substring(1, s.length()-1) : null;
    }
    String common(String a, String b) {
        if (assignable(a,b,new HashSet<>())) return a;
        if (assignable(b,a,new HashSet<>())) return b;
        if (a.startsWith("[") && b.startsWith("[")) {
            String x=component(a), y=component(b);
            if (x != null && y != null) { String base=common(x,y); return base.startsWith("[") ? "["+base : "[L"+base+";"; }
            return "java/lang/Object";
        }
        if (a.startsWith("[") || b.startsWith("[")) return "java/lang/Object";
        if (info(a).isInterface() || info(b).isInterface()) return "java/lang/Object";
        Set<String> seen = new HashSet<>();
        while (seen.add(a)) {
            a=info(a).parent();
            if (a==null) return "java/lang/Object";
            if (assignable(a,b,new HashSet<>())) return a;
        }
        throw new IllegalArgumentException("cyclic class hierarchy");
    }
    record FrameResult(byte[] bytes, List<String> failures) {}
    FrameResult frames(byte[] bytes) {
        // No ClassReader constructor: that optimization would copy unchanged
        // methods and leave dex2jar's missing StackMapTables untouched.
        ClassWriter writer = new ClassWriter(ClassWriter.COMPUTE_FRAMES) {
            @Override protected String getCommonSuperClass(String a, String b) { return common(a,b); }
        };
        List<String> failures = new ArrayList<>();
        ClassVisitor visitor = new ClassVisitor(Opcodes.ASM9, writer) {
            @Override public MethodVisitor visitMethod(int access, String name, String descriptor, String sig, String[] exceptions) {
                return new MethodVisitor(Opcodes.ASM9, super.visitMethod(access,name,descriptor,sig,exceptions)) {
                    @Override public void visitMaxs(int stack, int locals) {
                        try { super.visitMaxs(stack,locals); }
                        catch (RuntimeException e) { failures.add(name + descriptor + "\t" + e.toString().replace('\n',' ')); }
                    }
                };
            }
        };
        new ClassReader(bytes).accept(visitor, ClassReader.SKIP_FRAMES);
        if (failures.isEmpty()) return new FrameResult(writer.toByteArray(), failures);
        return new FrameResult(stubbed(bytes, failures), failures);
    }
    /** ART verifies a method that names a missing class softly: the class loads and
     * only that method throws NoClassDefFoundError when it runs. HotSpot rejects the
     * whole class without frames, so methods whose frames failed become that throw. */
    byte[] stubbed(byte[] bytes, List<String> failures) {
        Map<String,String> failed = new HashMap<>();
        for (String f : failures) {
            String method = f.substring(0, f.indexOf('\t'));
            java.util.regex.Matcher m = java.util.regex.Pattern.compile("missing frame hierarchy: (\\S+)").matcher(f);
            failed.putIfAbsent(method, m.find() ? m.group(1).replace('/', '.') : "unverifiable method " + method);
        }
        ClassWriter writer = new ClassWriter(ClassWriter.COMPUTE_FRAMES) {
            @Override protected String getCommonSuperClass(String a, String b) { return common(a,b); }
        };
        new ClassReader(bytes).accept(new ClassVisitor(Opcodes.ASM9, writer) {
            @Override public MethodVisitor visitMethod(int access, String name, String descriptor, String sig, String[] exceptions) {
                String missing = failed.get(name + descriptor);
                MethodVisitor mv = super.visitMethod(access, name, descriptor, sig, exceptions);
                if (missing == null) return mv;
                mv.visitCode();
                mv.visitTypeInsn(Opcodes.NEW, "java/lang/NoClassDefFoundError");
                mv.visitInsn(Opcodes.DUP);
                mv.visitLdcInsn(missing);
                mv.visitMethodInsn(Opcodes.INVOKESPECIAL, "java/lang/NoClassDefFoundError", "<init>", "(Ljava/lang/String;)V", false);
                mv.visitInsn(Opcodes.ATHROW);
                mv.visitMaxs(0, 0);
                mv.visitEnd();
                return null;
            }
        }, ClassReader.SKIP_FRAMES);
        return writer.toByteArray();
    }
}
