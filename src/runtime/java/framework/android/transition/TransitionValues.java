package android.transition;

import android.view.View;
import java.util.HashMap;
import java.util.Map;

/** AOSP core/java/android/transition/TransitionValues.java. */
public class TransitionValues {
  public View view;
  public final Map<String, Object> values = new HashMap<>();
  public TransitionValues() {}
  public TransitionValues(View view) { this.view = view; }
  @Override public boolean equals(Object other) {
    return other instanceof TransitionValues values && view == values.view && this.values.equals(values.values);
  }
  @Override public int hashCode() { return 31 * System.identityHashCode(view) + values.hashCode(); }
  @Override public String toString() {
    var result = new StringBuilder("TransitionValues@").append(Integer.toHexString(hashCode())).append(":\n")
        .append("    view = ").append(view).append("\n").append("    values:");
    for (var entry : values.entrySet())
      result.append("    ").append(entry.getKey()).append(": ").append(entry.getValue()).append("\n");
    return result.toString();
  }
}
