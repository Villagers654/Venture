package android.graphics.drawable;

import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Bitmap;
import android.graphics.PorterDuff;
import android.net.Uri;
import android.os.Parcel;
import android.os.Parcelable;

/** AOSP android.graphics.drawable.Icon: a reference to an image by resource, bitmap, data or URI. */
public final class Icon implements Parcelable {
  public static final int TYPE_BITMAP = 1, TYPE_RESOURCE = 2, TYPE_DATA = 3, TYPE_URI = 4,
      TYPE_ADAPTIVE_BITMAP = 5, TYPE_URI_ADAPTIVE_BITMAP = 6;
  private final int type;
  private Bitmap bitmap;
  private String resPackage, uri;
  private int resId, dataOffset, dataLength;
  private byte[] data;
  private ColorStateList tint;
  private PorterDuff.Mode tintMode = PorterDuff.Mode.SRC_IN;

  private Icon(int type) { this.type = type; }
  public static Icon createWithResource(Context context, int resId) {
    if (context == null) throw new IllegalArgumentException("Context must not be null.");
    return createWithResource(context.getPackageName(), resId);
  }
  public static Icon createWithResource(String resPackage, int resId) {
    if (resPackage == null) throw new IllegalArgumentException("Resource package name must not be null.");
    Icon icon = new Icon(TYPE_RESOURCE);
    icon.resPackage = resPackage;
    icon.resId = resId;
    return icon;
  }
  public static Icon createWithBitmap(Bitmap bitmap) { return bitmap(TYPE_BITMAP, bitmap); }
  public static Icon createWithAdaptiveBitmap(Bitmap bitmap) { return bitmap(TYPE_ADAPTIVE_BITMAP, bitmap); }
  private static Icon bitmap(int type, Bitmap bitmap) {
    if (bitmap == null) throw new IllegalArgumentException("Bitmap must not be null.");
    Icon icon = new Icon(type);
    icon.bitmap = bitmap;
    return icon;
  }
  public static Icon createWithData(byte[] data, int offset, int length) {
    if (data == null) throw new IllegalArgumentException("Data must not be null.");
    Icon icon = new Icon(TYPE_DATA);
    icon.data = data; icon.dataOffset = offset; icon.dataLength = length;
    return icon;
  }
  public static Icon createWithContentUri(String uri) { return uri(TYPE_URI, uri); }
  public static Icon createWithContentUri(Uri uri) {
    if (uri == null) throw new IllegalArgumentException("Uri must not be null.");
    return uri(TYPE_URI, uri.toString());
  }
  public static Icon createWithAdaptiveBitmapContentUri(String uri) { return uri(TYPE_URI_ADAPTIVE_BITMAP, uri); }
  public static Icon createWithAdaptiveBitmapContentUri(Uri uri) {
    if (uri == null) throw new IllegalArgumentException("Uri must not be null.");
    return uri(TYPE_URI_ADAPTIVE_BITMAP, uri.toString());
  }
  public static Icon createWithFilePath(String path) { return uri(TYPE_URI, path); }
  private static Icon uri(int type, String uri) {
    if (uri == null) throw new IllegalArgumentException("Uri must not be null.");
    Icon icon = new Icon(type);
    icon.uri = uri;
    return icon;
  }
  public int getType() { return type; }
  private void require(int... types) {
    for (int t : types) if (type == t) return;
    throw new IllegalStateException("called on the wrong type of Icon: " + type);
  }
  public int getResId() { require(TYPE_RESOURCE); return resId; }
  public String getResPackage() { require(TYPE_RESOURCE); return resPackage; }
  public Uri getUri() { require(TYPE_URI, TYPE_URI_ADAPTIVE_BITMAP); return Uri.parse(uri); }
  public Icon setTint(int tint) { return setTintList(ColorStateList.valueOf(tint)); }
  public Icon setTintList(ColorStateList tintList) { tint = tintList; return this; }
  public Icon setTintMode(PorterDuff.Mode mode) { tintMode = mode; return this; }
  public int describeContents() { return 0; }
  public void writeToParcel(Parcel dest, int flags) { dest.writeParcelable(this, flags); }
  public static final Creator<Icon> CREATOR = new Creator<>() {
    public Icon createFromParcel(Parcel in) { return in.readParcelable(null); }
    public Icon[] newArray(int size) { return new Icon[size]; }
  };
}
