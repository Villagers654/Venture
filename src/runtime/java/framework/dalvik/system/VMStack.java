package dalvik.system;

import java.lang.StackWalker;
import java.util.Arrays;
import java.util.Objects;

/** Android's stack inspection helpers, implemented using the host VM's stack API. */
public final class VMStack {
  private VMStack() {}
  private static final StackWalker WALKER = StackWalker.getInstance(StackWalker.Option.RETAIN_CLASS_REFERENCE);

  private static Class<?> caller(int depth) {
    return WALKER.walk(frames -> frames.map(StackWalker.StackFrame::getDeclaringClass)
        .filter(type -> type != VMStack.class).skip(depth).findFirst().orElse(null));
  }

  public static Class<?> getStackClass1() { return caller(1); }
  public static Class<?> getStackClass2() { return caller(2); }

  public static ClassLoader getCallingClassLoader() {
    Class<?> type = caller(1);
    return type == null ? null : type.getClassLoader();
  }

  public static ClassLoader getClosestUserClassLoader() {
    return WALKER.walk(frames -> frames.map(StackWalker.StackFrame::getDeclaringClass)
        .filter(type -> type != VMStack.class).map(Class::getClassLoader)
        .filter(Objects::nonNull).findFirst().orElse(null));
  }

  public static StackTraceElement[] getThreadStackTrace(Thread thread) {
    Objects.requireNonNull(thread);
    StackTraceElement[] trace = thread.getStackTrace();
    return trace.length == 0 ? null : trace;
  }

  public static int fillStackTraceElements(Thread thread, StackTraceElement[] destination) {
    Objects.requireNonNull(destination);
    StackTraceElement[] trace = getThreadStackTrace(thread);
    int count = trace == null ? 0 : Math.min(trace.length, destination.length);
    if (count != 0) System.arraycopy(trace, 0, destination, 0, count);
    Arrays.fill(destination, count, destination.length, null);
    return count;
  }
}
