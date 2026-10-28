import com.venture.runtime.*;
import java.io.*;
import java.nio.file.*;
import java.util.*;

/** The opt-in observer must preserve stream operations and bounded reads. */
public final class AssetReadTraceTest {
  private static int checks;
  private static void check(boolean ok) { ++checks; if (!ok) throw new AssertionError("Check " + checks); }
  public static void main(String[] args) throws Exception {
    var logs = new ArrayList<String>();
    Host host = (Host) java.lang.reflect.Proxy.newProxyInstance(Host.class.getClassLoader(), new Class<?>[]{Host.class},
        (proxy, method, values) -> {
          if (method.getName().equals("readAsset")) return "AndroidManifest.xml".equals(values[0]) ? FrameworkTest.obbManifest() : null;
          if (method.getName().equals("log")) {
            if (values[1].equals("Assets")) logs.add(values[2].toString());
            return null;
          }
          throw new UnsupportedOperationException(method.getName());
        });
    Path root = Files.createTempDirectory("asset-trace-test");
    RuntimeEnvironment.configure(new String[]{"test.game", root.resolve("base.apk").toString(), root.resolve("data").toString(),
        root.resolve("cache").toString(), root.resolve("obb").toString(), root.resolve("lib").toString(), "32", root.resolve("app.jar").toString()}, host);
    boolean[] closed = {false};
    var source = new ByteArrayInputStream(new byte[]{1,2,3,4,5}) {
      public void close() { closed[0] = true; }
    };
    var input = AssetReadTrace.opened(source, AssetReadTrace.begin("assets/fixture.bin"));
    check(input.markSupported() && input.available() == 5);
    check(input.read(new byte[0]) == 0 && logs.isEmpty());
    input.mark(100);
    check(input.read() == 1 && input.skip(2) == 2 && input.read() == 4);
    input.reset();
    check(Arrays.equals(input.readNBytes(2), new byte[]{1,2}));
    var output = new ByteArrayOutputStream();
    check(input.transferTo(output) == 3 && Arrays.equals(output.toByteArray(), new byte[]{3,4,5}));
    check(input.read() == -1 && input.available() == 0);
    input.close(); input.close();
    check(closed[0] && logs.size() == 2 && logs.get(0).startsWith("phase=first ") && logs.get(1).startsWith("phase=close "));
    check(logs.stream().noneMatch(line -> line.contains("fixture.bin")));
    check(logs.get(1).contains("open_allocated_bytes=") && logs.get(1).contains("observed_peak_heap_bytes="));
    var sha = java.security.MessageDigest.getInstance("SHA-256");
    check(logs.get(1).contains("read_sha256=" + java.util.HexFormat.of().formatHex(
        sha.digest(new byte[]{1,4,1,2,3,4,5}))));
    System.out.println("AssetReadTraceTest: " + checks + " checks passed");
  }
}
