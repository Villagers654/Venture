package android.content;

/**
 * AOSP android.content.MutableContextWrapper (API 11): a ContextWrapper whose base
 * context can be swapped later. Ad SDKs create their WebView controller on one and
 * move it between the application and the current activity.
 */
public class MutableContextWrapper extends ContextWrapper {
  public MutableContextWrapper(Context base) {
    super(base);
  }
  /** Changes the base context for this ContextWrapper; null is allowed, as in AOSP. */
  public void setBaseContext(Context base) {
    replaceBaseContext(base);
  }
}
