package android.util;

/** Inclusive immutable interval, AOSP core/java/android/util/Range.java. */
public final class Range<T extends Comparable<? super T>> {
  private final T lower, upper;
  public Range(T lower, T upper) {
    this.lower = java.util.Objects.requireNonNull(lower);
    this.upper = java.util.Objects.requireNonNull(upper);
    if (lower.compareTo(upper) > 0) throw new IllegalArgumentException("lower must be less than or equal to upper");
  }
  public static <T extends Comparable<? super T>> Range<T> create(T lower, T upper) { return new Range<>(lower, upper); }
  public T getLower() { return lower; }
  public T getUpper() { return upper; }
  public boolean contains(T value) { return value.compareTo(lower) >= 0 && value.compareTo(upper) <= 0; }
  public boolean contains(Range<T> value) { return contains(value.lower) && contains(value.upper); }
  public T clamp(T value) { return value.compareTo(lower) < 0 ? lower : value.compareTo(upper) > 0 ? upper : value; }
  public Range<T> intersect(Range<T> range) {
    if (range.contains(this)) return this;
    if (contains(range)) return range;
    return intersect(range.lower, range.upper);
  }
  public Range<T> intersect(T lo, T hi) {
    if (lo.compareTo(lower) <= 0 && hi.compareTo(upper) >= 0) return this;
    return new Range<>(lo.compareTo(lower) > 0 ? lo : lower, hi.compareTo(upper) < 0 ? hi : upper);
  }
  public Range<T> extend(Range<T> range) {
    if (contains(range)) return this;
    if (range.contains(this)) return range;
    return extend(range.lower, range.upper);
  }
  public Range<T> extend(T value) { return extend(value, value); }
  public Range<T> extend(T lo, T hi) {
    if (lo.compareTo(lower) >= 0 && hi.compareTo(upper) <= 0) return this;
    return new Range<>(lo.compareTo(lower) < 0 ? lo : lower, hi.compareTo(upper) > 0 ? hi : upper);
  }
  @Override public boolean equals(Object other) {
    return other instanceof Range<?> r && lower.equals(r.lower) && upper.equals(r.upper);
  }
  @Override public int hashCode() {
    // AOSP HashCodeHelpers.hashCodeGeneric: seed 1, h = (31 * h) XOR element.
    return (31 * (31 ^ lower.hashCode())) ^ upper.hashCode();
  }
  @Override public String toString() { return "[" + lower + ", " + upper + "]"; }
}
