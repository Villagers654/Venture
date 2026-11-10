package android.text.method;

// AOSP android.text.method.MovementMethod: how a TextView moves its cursor and
// scrolls. Venture keeps the method a TextView was given.
public interface MovementMethod {
  default void initialize(android.widget.TextView widget, android.text.Spannable text) {}
  default boolean canSelectArbitrarily() { return false; }
}
