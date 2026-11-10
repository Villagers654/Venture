package android.text.method;

public class ScrollingMovementMethod implements MovementMethod {
  private static ScrollingMovementMethod instance;
  public static synchronized MovementMethod getInstance() {
    if (instance == null) instance = new ScrollingMovementMethod();
    return instance;
  }
}
