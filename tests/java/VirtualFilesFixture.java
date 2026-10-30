import com.venture.runtime.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.concurrent.atomic.AtomicInteger;

/** Execute only after the production APK rewriter processes this class. */
public class VirtualFilesFixture {
  static void check(boolean value) { if (!value) throw new AssertionError("virtual file contract"); }
  static class HostStream extends FileInputStream {
    HostStream(File file) throws IOException { super(file); }
  }
  static class CheckedFile extends File {
    CheckedFile(String path) { super(path); }
    // invokespecial File.exists stays non-virtual; a rewrite would recurse here.
    @Override public boolean exists() { return super.exists(); }
  }
  public static void main(String[] args) throws Exception {
    var reads = new AtomicInteger();
    Host host = (Host) java.lang.reflect.Proxy.newProxyInstance(Host.class.getClassLoader(), new Class<?>[]{Host.class},
        (object, method, values) -> {
          if (method.getName().equals("log")) throw new AssertionError("snapshot cleanup failed");
          String path = new String((byte[])values[0], StandardCharsets.UTF_8);
          if (method.getName().equals("virtualFileExists")) return path.equals("/proc/self/status") || path.equals("/proc");
          if (method.getName().equals("readVirtualFile"))
            return path.equals("/proc/self/status") ? ("VmRSS: " + reads.incrementAndGet() + " kB\n\u96ea\n").getBytes(StandardCharsets.UTF_8) : null;
          throw new AssertionError("unexpected host call");
        });
    var boundary = RuntimeEnvironment.class.getDeclaredField("host");
    boundary.setAccessible(true); boundary.set(null, host);
    check(new File("/proc/self/status").exists() && new File("/proc").exists());
    check(!new File("/proc/missing").exists());
    try (var first = new BufferedReader(new FileReader("/proc/self/status", StandardCharsets.UTF_8));
         var second = new FileInputStream(new File("/proc/self/status"))) {
      check(first.readLine().equals("VmRSS: 1 kB") && first.readLine().equals("\u96ea"));
      check(new String(second.readAllBytes(), StandardCharsets.UTF_8).startsWith("VmRSS: 2 kB"));
      second.getChannel().position(0); check(second.read() == 'V');
    }
    try (var third = new BufferedReader(new FileReader(new File("/proc/self/status")))) {
      check(third.readLine().equals("VmRSS: 3 kB"));
    }
    try { new FileReader("/proc/missing"); throw new AssertionError("missing virtual file"); }
    catch (FileNotFoundException expected) {}
    Path nativeFile = Path.of(args[0], "ordinary.txt"); Files.writeString(nativeFile, "host file");
    check(nativeFile.toFile().exists());
    File checked = new CheckedFile(nativeFile.toString());
    check(checked.exists());
    try (var input = new HostStream(nativeFile.toFile()); var reader = new FileReader(input.getFD())) {
      check(reader.read() == 'h');
    }
    try (var input = new FileInputStream(nativeFile.toString())) {
      check(new String(input.readAllBytes(), StandardCharsets.UTF_8).equals("host file"));
    }
    // Host paths pass through the rewritten guest-path constructors unchanged.
    try (var file = new RandomAccessFile(nativeFile.toString(), "r")) { check(file.read() == 'h'); }
    Path written = Path.of(args[0], "written.txt");
    try (var output = new FileOutputStream(written.toString(), false)) { output.write('w'); }
    check(new File(written.toString()).length() == 1);
    check(ItfCaller.call().equals("interface static"));
    try (var snapshots = Files.list(Path.of(System.getProperty("java.io.tmpdir")))) {
      check(snapshots.findAny().isEmpty());
    }
    System.out.println("PASS rewritten virtual reads, fresh independent snapshots, host files, interface calls and cleanup");
  }
}
