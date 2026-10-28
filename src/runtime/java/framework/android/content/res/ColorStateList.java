package android.content.res;
public class ColorStateList {
  private final int[][] states;
  private final int[] colors;
  public ColorStateList(int[][] s, int[] c) {
    states = java.util.Arrays.stream(s).map(int[] ::clone).toArray(int[][] ::new);
    colors = c.clone();
    if (s.length != c.length || c.length == 0)
      throw new IllegalArgumentException();
  }
  public static ColorStateList valueOf(int color) {
    return new ColorStateList(new int[][] {{}}, new int[] {color});
  }
  public int getDefaultColor() {
    for (int i = 0; i < states.length; i++)
      if (states[i].length == 0)
        return colors[i];
    return colors[0];
  }
  public boolean isStateful() {
    return states.length > 1 || states[0].length > 0;
  }
  public int getColorForState(int[] state, int fallback) {
  outer:
    for (int i = 0; i < states.length; i++) {
      for (int required : states[i]) {
        boolean found = false;
        for (int v : state)
          if (v == Math.abs(required))
            found = true;
        if (found != (required > 0))
          continue outer;
      }
      return colors[i];
    }
    return fallback;
  }
}
