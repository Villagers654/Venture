package android.text.method;

public class LinkMovementMethod extends ScrollingMovementMethod {
  private static LinkMovementMethod instance;
  public static synchronized MovementMethod getInstance() {
    if (instance == null) instance = new LinkMovementMethod();
    return instance;
  }
}
