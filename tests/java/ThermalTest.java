import android.content.Context;
import android.os.Looper;
import android.os.PowerManager;
import java.util.ArrayList;
import java.util.concurrent.RejectedExecutionException;

public final class ThermalTest {
  private static void check(boolean value) {
    if (!value) throw new AssertionError();
  }
  public static void main(String[] args) throws Exception {
    var warnings = new ArrayList<String>();
    var host = com.venture.runtime.RuntimeEnvironment.class.getDeclaredField("host");
    host.setAccessible(true);
    host.set(null, java.lang.reflect.Proxy.newProxyInstance(ThermalTest.class.getClassLoader(),
        new Class<?>[]{com.venture.runtime.Host.class}, (proxy, method, values) -> {
          if (!method.getName().equals("log")) throw new UnsupportedOperationException(method.getName());
          warnings.add((String)values[2]); return null;
        }));
    Looper.prepareMainLooper();
    var executor = new Context().getMainExecutor();
    var manager = new PowerManager();
    var calls = new ArrayList<Integer>();
    var pending = new ArrayList<Runnable>();
    PowerManager.OnThermalStatusChangedListener listener = calls::add;
    manager.addThermalStatusListener(pending::add, listener);
    try { manager.addThermalStatusListener(pending::add, listener); throw new AssertionError(); }
    catch (IllegalArgumentException expected) {}
    try { manager.addThermalStatusListener(null, listener); throw new AssertionError(); }
    catch (NullPointerException expected) {}
    try { manager.addThermalStatusListener(executor, null); throw new AssertionError(); }
    catch (NullPointerException expected) {}
    check(calls.isEmpty() && pending.isEmpty());
    var mainThread = Thread.currentThread();
    var worker = new Thread(() -> executor.execute(() -> {
      check(Thread.currentThread() == mainThread);
      check(pending.size() == 1);
      pending.removeFirst().run();
      check(calls.equals(java.util.List.of(PowerManager.THERMAL_STATUS_NONE)));
      check(manager.getCurrentThermalStatus() == calls.getFirst());
      manager.removeThermalStatusListener(listener);
      try { manager.removeThermalStatusListener(listener); throw new AssertionError(); }
      catch (IllegalArgumentException expected) {}
      manager.addThermalStatusListener(status -> {
        check(Thread.currentThread() == mainThread && status == PowerManager.THERMAL_STATUS_NONE);
        Looper.myLooper().quit();
      });
    }));
    worker.start(); worker.join();
    Looper.loop();
    try { executor.execute(() -> {}); throw new AssertionError(); }
    catch (RejectedExecutionException expected) {}
    check(warnings.size() == 1 && warnings.getFirst().contains("No Android thermal HAL"));
    System.out.println("PASS thermal listener lifecycle and main executor");
  }
}
