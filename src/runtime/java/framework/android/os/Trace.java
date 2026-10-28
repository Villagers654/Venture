package android.os;
public final class Trace {
  private static final ThreadLocal<java.util.Deque<String>> sections =
      ThreadLocal.withInitial(java.util.ArrayDeque::new);
  public static void beginSection(String name) {
    sections.get().push(name);
    com.venture.runtime.RuntimeEnvironment.trace("Trace.begin(" + name + ")");
  }
  public static void endSection() {
    if (!sections.get().isEmpty())
      sections.get().pop();
  }
  public static boolean isEnabled() {
    return Boolean.getBoolean("venture.trace.framework");
  }
  public static boolean isTagEnabled(long tag) {
    return isEnabled();
  }
}
