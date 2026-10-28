public final class TestNatives {
  public static volatile String reached;
  public static synchronized void framework(String method) {
    System.out.println("FRAMEWORK_CALL [" + Thread.currentThread().getName() + "] " + method);
  }
  public static final class Boundary extends Error {
    public Boundary(String method) {
      super(method);
    }
  }
  public static void enter(String method) {
    System.out.println("GUEST_NATIVE " + method);
    if (method.startsWith("com/unity3d/player/NativeLoader.load"))
      return;
    reached = method;
    throw new Boundary(method);
  }
}
