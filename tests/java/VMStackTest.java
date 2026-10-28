import dalvik.system.VMStack;
import java.util.Arrays;

public class VMStackTest {
  static class Inner {
    static Class<?> one() { return VMStack.getStackClass1(); }
    static Class<?> two() { return Middle.two(); }
    static ClassLoader loader() { return VMStack.getCallingClassLoader(); }
  }
  static class Middle {
    static Class<?> two() { return VMStack.getStackClass2(); }
  }
  static void check(boolean condition) {
    if (!condition) throw new AssertionError();
  }
  public static void main(String[] args) throws Exception {
    check(Inner.one() == VMStackTest.class);
    check(Inner.two() == VMStackTest.class);
    check(Inner.loader() == VMStackTest.class.getClassLoader());
    check(VMStack.getClosestUserClassLoader() == VMStackTest.class.getClassLoader());
    check(VMStack.getThreadStackTrace(Thread.currentThread()).length > 0);
    Thread thread = new Thread(() -> {});
    check(VMStack.getThreadStackTrace(thread) == null);
    thread.start(); thread.join();
    check(VMStack.getThreadStackTrace(thread) == null);
    StackTraceElement[] destination = new StackTraceElement[64];
    Arrays.fill(destination, new StackTraceElement("old", "old", "old", 1));
    int count = VMStack.fillStackTraceElements(Thread.currentThread(), destination);
    check(count > 0 && count < destination.length);
    for (int i = 0; i < destination.length; ++i) check((destination[i] != null) == (i < count));
    check(VMStack.fillStackTraceElements(thread, destination) == 0);
    for (var value : destination) check(value == null);
    check(VMStack.fillStackTraceElements(Thread.currentThread(), new StackTraceElement[0]) == 0);
    System.out.println("VMStack stack depth, classloader, thread lifetime and trace bounds passed");
  }
}
