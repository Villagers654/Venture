package android.util;

/** Finite dimensions, AOSP core/java/android/util/SizeF.java. */
public final class SizeF implements android.os.Parcelable {
  private final float width, height;
  public SizeF(float width, float height) {
    if (!Float.isFinite(width) || !Float.isFinite(height)) throw new IllegalArgumentException("Non-finite size");
    this.width = width; this.height = height;
  }
  public float getWidth() { return width; }
  public float getHeight() { return height; }
  @Override public boolean equals(Object other) { return other instanceof SizeF s && width == s.width && height == s.height; }
  @Override public int hashCode() { return Float.floatToIntBits(width) ^ Float.floatToIntBits(height); }
  @Override public String toString() { return width + "x" + height; }
  public int describeContents() { return 0; }
  public void writeToParcel(android.os.Parcel out, int flags) { out.writeFloat(width); out.writeFloat(height); }
  public static final android.os.Parcelable.Creator<SizeF> CREATOR = new android.os.Parcelable.Creator<>() {
    public SizeF createFromParcel(android.os.Parcel in) { return new SizeF(in.readFloat(), in.readFloat()); }
    public SizeF[] newArray(int size) { return new SizeF[size]; }
  };
  public static SizeF parseSizeF(String value) {
    java.util.Objects.requireNonNull(value, "string must not be null");
    int split = value.indexOf('*');
    if (split < 0) split = value.indexOf('x');
    if (split < 0) throw new NumberFormatException("Invalid SizeF: " + value);
    try { return new SizeF(Float.parseFloat(value.substring(0, split)), Float.parseFloat(value.substring(split + 1))); }
    catch (IllegalArgumentException invalid) { throw new NumberFormatException("Invalid SizeF: " + value); }
  }
}
