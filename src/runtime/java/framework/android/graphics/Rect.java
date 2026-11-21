package android.graphics;
public class Rect implements android.os.Parcelable {
  public int left, top, right, bottom;
  public Rect() {}
  public Rect(Rect r) {
    if (r != null)
      set(r.left, r.top, r.right, r.bottom);
  }
  public Rect(int l, int t, int r, int b) {
    set(l, t, r, b);
  }
  public void offset(int dx, int dy) { left += dx; right += dx; top += dy; bottom += dy; }
  public void set(int l, int t, int r, int b) {
    left = l;
    top = t;
    right = r;
    bottom = b;
  }
  public void set(Rect src) {
    set(src.left, src.top, src.right, src.bottom);
  }
  public void setEmpty() {
    set(0, 0, 0, 0);
  }
  public boolean equals(Object o) {
    return o instanceof Rect r && left == r.left && top == r.top && right == r.right && bottom == r.bottom;
  }
  public int hashCode() {
    return 31 * (31 * (31 * left + top) + right) + bottom;
  }
  public int width() {
    return right - left;
  }
  public int height() {
    return bottom - top;
  }
  public boolean isEmpty() {
    return width() <= 0 || height() <= 0;
  }
  // Empty rectangles contain nothing; right and bottom are exclusive.
  public boolean contains(int x, int y) {
    return left < right && top < bottom && x >= left && x < right && y >= top && y < bottom;
  }
  public int describeContents() { return 0; }
  public void writeToParcel(android.os.Parcel out, int flags) {
    out.writeInt(left);
    out.writeInt(top);
    out.writeInt(right);
    out.writeInt(bottom);
  }
  public void readFromParcel(android.os.Parcel in) {
    set(in.readInt(), in.readInt(), in.readInt(), in.readInt());
  }
  public static final android.os.Parcelable.Creator<Rect> CREATOR = new android.os.Parcelable.Creator<>() {
    public Rect createFromParcel(android.os.Parcel in) {
      Rect r = new Rect();
      r.readFromParcel(in);
      return r;
    }
    public Rect[] newArray(int size) { return new Rect[size]; }
  };
}
