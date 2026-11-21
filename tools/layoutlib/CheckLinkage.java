// Venture: finds the members layoutlib's framework code uses that a 2D process's
// Venture classes do not have (docs/design/layoutlib.md).
//
// Usage: java -cp asm.jar CheckLinkage.java <layoutlib.jar> <venture-framework-2d.jar> [allowed.txt...]
//
// A 2D process runs layoutlib's Android framework with some of Venture's classes
// in place of layoutlib's (tools/layoutlib/select_sources.py). Code on either
// side that names a field or method the other side's class lacks fails at run
// time with NoSuchFieldError or NoSuchMethodError, often deep inside a layout
// pass. This walks every field and method reference in both jars, resolves it
// the way the JVM does (the class, its superclasses, then its interfaces), and
// prints the ones that resolve nowhere, grouped by the class that lacks them;
// also member classes neither side has whose outer class one side has
// ("owner.class Member" in allowed.txt).
// Lines of allowed.txt ("owner.name descriptor", '#' comments) are references
// known never to run; a line "from <regex>" allows every reference made by the
// classes whose names match (code a 2D process never reaches). The exit status
// is 1 when a reference is missing.
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.TreeSet;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import org.objectweb.asm.ClassReader;
import org.objectweb.asm.ClassVisitor;
import org.objectweb.asm.FieldVisitor;
import org.objectweb.asm.MethodVisitor;
import org.objectweb.asm.Opcodes;

public class CheckLinkage {
  record Info(String name, String superName, String[] interfaces, Set<String> members, boolean fromVenture) {}

  static final Map<String, Info> classes = new HashMap<>();

  public static void main(String[] args) throws Exception {
    Map<String, byte[]> bytes = new HashMap<>();
    // Venture's jar comes first on the class path, so its classes win.
    load(Path.of(args[1]), true, bytes);
    load(Path.of(args[0]), false, bytes);
    Set<String> allowed = new HashSet<>();
    List<java.util.regex.Pattern> allowedUsers = new ArrayList<>();
    for (int i = 2; i < args.length; ++i)
      for (String line : Files.readAllLines(Path.of(args[i]))) {
        line = line.replaceAll("#.*", "").trim();
        if (line.startsWith("from ")) allowedUsers.add(java.util.regex.Pattern.compile(line.substring(5).trim()));
        else if (!line.isEmpty()) allowed.add(line);
      }
    Map<String, Set<String>> missing = new TreeMap<>();
    Map<String, Set<String>> users = new HashMap<>();
    for (var entry : bytes.entrySet()) {
      String user = entry.getKey();
      boolean userVenture = classes.get(user).fromVenture();
      String userName = user.replace('/', '.');
      if (allowedUsers.stream().anyMatch(p -> p.matcher(userName).lookingAt())) continue;
      new ClassReader(entry.getValue()).accept(new ClassVisitor(Opcodes.ASM9) {
        @Override
        public MethodVisitor visitMethod(int access, String name, String descriptor, String signature, String[] exceptions) {
          return new MethodVisitor(Opcodes.ASM9) {
            // A member class neither side has, of a class one side has (a member
            // class of a layoutlib class Venture replaced, that Venture lacks).
            boolean checkClass(String owner) {
              while (owner.startsWith("[")) owner = owner.substring(1);
              if (owner.startsWith("L") && owner.endsWith(";")) owner = owner.substring(1, owner.length() - 1);
              if (classes.containsKey(owner)) return true;
              int split = owner.lastIndexOf('$');
              if (split < 0 || !classes.containsKey(owner.substring(0, split))) return false;
              String outer = owner.substring(0, split);
              String line = outer.replace('/', '.') + ".class " + owner.substring(split + 1);
              if (allowed.contains(line)) return false;
              missing.computeIfAbsent(outer.replace('/', '.'), k -> new TreeSet<>()).add("class " + owner.substring(split + 1));
              users.computeIfAbsent(line, k -> new TreeSet<>()).add(user.replace('/', '.'));
              return false;
            }
            @Override
            public void visitTypeInsn(int opcode, String type) {
              checkClass(type);
            }
            void check(String owner, String member, String desc, boolean field) {
              if (owner.startsWith("[")) return;
              Info info = classes.get(owner);
              if (info == null) {  // a JDK class, or a class missing altogether
                checkClass(owner);
                return;
              }
              // Only references that cross between Venture's and layoutlib's classes.
              if (!crosses(owner, userVenture)) return;
              String key = member + (field ? ":" : "") + desc;
              if (resolve(owner, key, new HashSet<>())) return;
              String line = owner.replace('/', '.') + "." + member + " " + desc;
              if (allowed.contains(line)) return;
              missing.computeIfAbsent(owner.replace('/', '.'), k -> new TreeSet<>()).add(member + " " + desc);
              users.computeIfAbsent(line, k -> new TreeSet<>()).add(user.replace('/', '.'));
            }
            @Override
            public void visitFieldInsn(int opcode, String owner, String member, String desc) {
              check(owner, member, desc, true);
            }
            @Override
            public void visitMethodInsn(int opcode, String owner, String member, String desc, boolean itf) {
              check(owner, member, desc, false);
            }
          };
        }
      }, ClassReader.SKIP_DEBUG | ClassReader.SKIP_FRAMES);
    }
    int count = 0;
    for (var entry : missing.entrySet()) {
      System.out.println(entry.getKey() + (classes.get(entry.getKey().replace('.', '/')).fromVenture() ? " (Venture)" : " (layoutlib)"));
      for (String member : entry.getValue()) {
        ++count;
        Set<String> from = users.get(entry.getKey() + "." + member);
        List<String> shown = new ArrayList<>(from);
        System.out.println("  " + member + "  <- " + String.join(", ", shown.subList(0, Math.min(3, shown.size())))
            + (shown.size() > 3 ? " +" + (shown.size() - 3) : ""));
      }
    }
    System.out.println(count + " missing members");
    System.exit(count == 0 ? 0 : 1);
  }

  /** Whether a reference from a Venture (or layoutlib) class to owner crosses sides. */
  static boolean crosses(String owner, boolean userVenture) {
    // A reference resolves through the owner's superclasses too, so it crosses
    // when any class on that chain comes from the other side.
    for (String c = owner; c != null; ) {
      Info info = classes.get(c);
      if (info == null) return false;
      if (info.fromVenture() != userVenture) return true;
      c = info.superName();
    }
    return false;
  }

  static boolean resolve(String owner, String key, Set<String> seen) {
    if (owner == null || !seen.add(owner)) return false;
    Info info = classes.containsKey(owner) ? classes.get(owner) : jdk(owner);
    if (info == null) return true;  // a class missing altogether is reported elsewhere
    if (info.members().contains(key)) return true;
    if (key.startsWith("<init>")) return false;
    if (resolve(info.superName(), key, seen)) return true;
    for (String i : info.interfaces()) if (resolve(i, key, seen)) return true;
    return false;
  }

  static final Map<String, Info> jdkClasses = new HashMap<>();

  /** A JDK class (java.lang.Object, ...) as the running JDK has it; null if there is none. */
  static Info jdk(String owner) {
    return jdkClasses.computeIfAbsent(owner, name -> {
      try (var in = ClassLoader.getSystemResourceAsStream(name + ".class")) {
        if (in == null) return null;
        ClassReader reader = new ClassReader(in.readAllBytes());
        return new Info(name, reader.getSuperName(), reader.getInterfaces(), members(reader), false);
      } catch (java.io.IOException e) {
        throw new java.io.UncheckedIOException(e);
      }
    });
  }

  static Set<String> members(ClassReader reader) {
    Set<String> members = new HashSet<>();
    reader.accept(new ClassVisitor(Opcodes.ASM9) {
      @Override
      public FieldVisitor visitField(int access, String name, String descriptor, String signature, Object value) {
        members.add(name + ":" + descriptor);
        return null;
      }
      @Override
      public MethodVisitor visitMethod(int access, String name, String descriptor, String signature, String[] exceptions) {
        members.add(name + descriptor);
        return null;
      }
    }, ClassReader.SKIP_CODE);
    return members;
  }

  static void load(Path jar, boolean venture, Map<String, byte[]> bytes) throws Exception {
    try (ZipFile zip = new ZipFile(jar.toFile())) {
      for (var entries = zip.entries(); entries.hasMoreElements();) {
        ZipEntry entry = entries.nextElement();
        if (!entry.getName().endsWith(".class") || entry.getName().startsWith("META-INF/")) continue;
        byte[] data = zip.getInputStream(entry).readAllBytes();
        ClassReader reader = new ClassReader(data);
        if (classes.containsKey(reader.getClassName())) continue;
        Set<String> members = members(reader);
        classes.put(reader.getClassName(), new Info(reader.getClassName(), reader.getSuperName(), reader.getInterfaces(), members, venture));
        bytes.put(reader.getClassName(), data);
      }
    }
  }
}
