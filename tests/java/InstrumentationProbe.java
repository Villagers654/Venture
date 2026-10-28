import java.lang.instrument.Instrumentation;

public final class InstrumentationProbe {
  public static void premain(String options, Instrumentation instrumentation) {
    System.setProperty("venture.instrumentation.probe", "loaded");
  }

  public static void main(String[] args) {
    if (!"loaded".equals(System.getProperty("venture.instrumentation.probe")))
      throw new AssertionError("JNI startup did not load the Java agent");
    System.out.println("PASS JNI instrumentation startup");
  }
}
