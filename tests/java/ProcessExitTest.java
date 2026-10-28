import com.venture.runtime.*;
import java.lang.reflect.*;
import java.util.*;

/** A recording Host proves guest exits never reach HotSpot's process exit. */
public final class ProcessExitTest {
  private static final List<Integer> exits = new ArrayList<>();
  private static final List<String> decisions = new ArrayList<>();
  private static boolean deciding;
  private static int checks;
  private static void check(boolean value) {
    checks++;
    if (!value) throw new AssertionError("process exit check " + checks);
  }
  private static void terminated(Runnable action, int status) {
    int before = exits.size();
    boolean terminated = false;
    try { action.run(); } catch (ThreadDeath stopped) { terminated = true; }
    check(terminated && exits.size() == before + 1 && exits.get(before) == status);
  }
  public static void main(String[] args) throws Exception {
    Host recording = (Host) Proxy.newProxyInstance(Host.class.getClassLoader(), new Class<?>[]{Host.class},
        (object, method, values) -> {
          if (method.getName().equals("beginProcessExit")) {
            check(!deciding); deciding = true; decisions.add("begin"); return null;
          }
          if (method.getName().equals("endProcessExit")) {
            check(deciding); deciding = false; decisions.add("end"); return null;
          }
          if (method.getName().equals("exitProcess")) {
            check(deciding); decisions.add("exit");
            exits.add((Integer) values[0]); throw new ThreadDeath();
          }
          if (method.getName().equals("restartProcess")) {
            check(deciding); decisions.add("restart"); throw new ThreadDeath();
          }
          if (method.getName().equals("log")) return null;
          throw new AssertionError("unexpected Host call " + method);
        });
    var host = RuntimeEnvironment.class.getDeclaredField("host");
    host.setAccessible(true); host.set(null, recording);
    terminated(() -> NativeLoader.exit(-7), -7);
    terminated(() -> NativeLoader.exit(Runtime.getRuntime(), 23), 23);
    int before = exits.size();
    boolean nullRejected = false;
    try { NativeLoader.exit(null, 0); } catch (NullPointerException expected) { nullRejected = true; }
    check(nullRejected && exits.size() == before);
    var system = System.class.getMethod("exit", int.class);
    var runtime = Runtime.class.getMethod("exit", int.class);
    for (Object status : List.of((byte) 24, (short) 25, (char) 26, 27)) {
      int value = status instanceof Character c ? c : ((Number) status).intValue();
      before = exits.size();
      boolean wrapped = false;
      try { NativeLoader.invoke(system, null, new Object[]{status}); }
      catch (InvocationTargetException expected) { wrapped = expected.getCause() instanceof ThreadDeath; }
      check(wrapped && exits.size() == before + 1 && exits.get(before) == value);
    }
    boolean wrapped = false;
    try { NativeLoader.invoke(runtime, Runtime.getRuntime(), new Object[]{28}); }
    catch (InvocationTargetException expected) { wrapped = expected.getCause() instanceof ThreadDeath; }
    check(wrapped && exits.get(exits.size() - 1) == 28);
    for (Object invalid : new Object[]{null, 1L, 1f, "1"}) {
      boolean rejected = false;
      try { NativeLoader.invoke(system, null, new Object[]{invalid}); }
      catch (IllegalArgumentException expected) { rejected = true; }
      check(rejected);
    }
    if (args.length != 0) {
      try (var fixture = new java.net.URLClassLoader(new java.net.URL[]{new java.io.File(args[0]).toURI().toURL()},
          ProcessExitTest.class.getClassLoader())) {
        var type = fixture.loadClass("ProcessExitFixture");
        int status = 40;
        for (String name : List.of("system", "runtime", "staticHandle", "boundHandle", "unboundHandle", "reflected")) {
          before = exits.size();
          Throwable failure = null;
          try { type.getMethod(name, int.class).invoke(null, status); }
          catch (InvocationTargetException expected) { failure = expected.getCause(); }
          if (name.equals("reflected") && failure instanceof InvocationTargetException reflected)
            failure = reflected.getCause();
          check(failure instanceof ThreadDeath && exits.size() == before + 1 && exits.get(before) == status);
          status++;
        }
      }
    }
    // An onDestroy exit(0) must not erase an earlier Java startup failure.
    var failure = AppMain.class.getDeclaredField("failureStatus");
    failure.setAccessible(true); failure.setInt(null, 1);
    terminated(() -> NativeLoader.exit(0), 1);
    check(!deciding && decisions.size() == exits.size() * 3);
    for (int i = 0; i < decisions.size(); i += 3) {
      check(decisions.get(i).equals("begin"));
      check(decisions.get(i + 1).equals("exit") || decisions.get(i + 1).equals("restart"));
      check(decisions.get(i + 2).equals("end"));
    }
    System.out.println("PASS " + checks + " process exit checks");
  }
}
