package android.graphics;

/** Floating-point rectangle, with the same exclusive right/bottom edges as Rect. */
public class RectF implements android.os.Parcelable {
  public float left, top, right, bottom;
  public RectF() {}
  public RectF(float left, float top, float right, float bottom) { set(left, top, right, bottom); }
  public RectF(RectF rectangle) {
    if (rectangle != null) set(rectangle.left, rectangle.top, rectangle.right, rectangle.bottom);
  }
  public RectF(Rect rectangle) {
    if (rectangle != null) set(rectangle.left, rectangle.top, rectangle.right, rectangle.bottom);
  }
  public void set(float left, float top, float right, float bottom) {
    this.left = left; this.top = top; this.right = right; this.bottom = bottom;
  }
  public void set(RectF rectangle) { set(rectangle.left, rectangle.top, rectangle.right, rectangle.bottom); }
  public void set(Rect rectangle) { set(rectangle.left, rectangle.top, rectangle.right, rectangle.bottom); }
  public void setEmpty() { set(0, 0, 0, 0); }
  public float width() { return right - left; }
  public float height() { return bottom - top; }
  public float centerX() { return (left + right) * 0.5f; }
  public float centerY() { return (top + bottom) * 0.5f; }
  public boolean isEmpty() { return left >= right || top >= bottom; }
  public boolean contains(float x, float y) {
    return left < right && top < bottom && x >= left && x < right && y >= top && y < bottom;
  }
  public void offset(float dx, float dy) { left += dx; right += dx; top += dy; bottom += dy; }
  public void inset(float dx, float dy) { left += dx; right -= dx; top += dy; bottom -= dy; }
  public int describeContents() { return 0; }
  public void writeToParcel(android.os.Parcel parcel, int flags) {
    parcel.writeFloat(left); parcel.writeFloat(top); parcel.writeFloat(right); parcel.writeFloat(bottom);
  }
  public void readFromParcel(android.os.Parcel parcel) {
    set(parcel.readFloat(), parcel.readFloat(), parcel.readFloat(), parcel.readFloat());
  }
  public static final android.os.Parcelable.Creator<RectF> CREATOR = new android.os.Parcelable.Creator<>() {
    public RectF createFromParcel(android.os.Parcel parcel) {
      var rectangle = new RectF(); rectangle.readFromParcel(parcel); return rectangle;
    }
    public RectF[] newArray(int size) { return new RectF[size]; }
  };
}
