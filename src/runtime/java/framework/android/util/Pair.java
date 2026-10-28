package android.util;

/** Value pair, following AOSP core/java/android/util/Pair.java. */
public class Pair<F, S> {
  public final F first;
  public final S second;
  public Pair(F first, S second) { this.first = first; this.second = second; }
  public static <A, B> Pair<A, B> create(A first, B second) { return new Pair<>(first, second); }
  @Override public boolean equals(Object other) {
    return other instanceof Pair<?, ?> pair && java.util.Objects.equals(first, pair.first)
        && java.util.Objects.equals(second, pair.second);
  }
  @Override public int hashCode() { return java.util.Objects.hashCode(first) ^ java.util.Objects.hashCode(second); }
  @Override public String toString() { return "Pair{" + first + " " + second + "}"; }
}
