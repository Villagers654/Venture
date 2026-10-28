import com.venture.runtime.*;
import java.io.*;
import java.lang.reflect.Proxy;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.*;

public final class ProcessCommandsTest {
  private static void check(boolean condition) {
    if (!condition) throw new AssertionError();
  }
  public static void main(String[] args) throws Exception {
    var host = RuntimeEnvironment.class.getDeclaredField("host");
    host.setAccessible(true);
    host.set(null, Proxy.newProxyInstance(Host.class.getClassLoader(), new Class<?>[]{Host.class},
        (object, method, values) -> { if (method.getName().equals("log")) return null;
          throw new AssertionError("Unexpected host call " + method); }));
    var clear = ProcessCommands.start(new ProcessBuilder("logcat", "-c"));
    check(clear.waitFor(1, TimeUnit.SECONDS) && clear.exitValue() == 0);
    android.util.Log.i("ProcessTest", "before reader");
    var dump = ProcessCommands.start(new ProcessBuilder("/system/bin/logcat", "-d"));
    check(dump.waitFor(1, TimeUnit.SECONDS));
    check(new String(dump.getInputStream().readAllBytes(), StandardCharsets.UTF_8).contains("before reader"));
    ProcessCommands.start(new ProcessBuilder("logcat", "-c"));
    var live = ProcessCommands.start(new ProcessBuilder("logcat", "-v", "threadtime"));
    var executor = Executors.newSingleThreadExecutor();
    try {
      var reader = new BufferedReader(new InputStreamReader(live.getInputStream(), StandardCharsets.UTF_8));
      var read = executor.submit(() -> reader.readLine());
      android.util.Log.w("ProcessTest", "after reader");
      check(read.get(1, TimeUnit.SECONDS).contains("after reader"));
      var blocked = executor.submit(() -> reader.readLine());
      live.destroy();
      check(blocked.get(1, TimeUnit.SECONDS) == null);
      check(live.waitFor(1, TimeUnit.SECONDS));
    } finally { live.destroy(); executor.shutdownNow(); }
    var missing = "venture-command-that-does-not-exist";
    try { ProcessCommands.start(new ProcessBuilder(missing)); throw new AssertionError(); }
    catch (IOException expected) { check(expected.getMessage().contains(missing)); }
    System.out.println("ProcessCommandsTest passed");
  }
}
