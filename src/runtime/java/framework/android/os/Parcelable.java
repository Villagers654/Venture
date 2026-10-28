package android.os;
public interface Parcelable {
  int CONTENTS_FILE_DESCRIPTOR = 1, PARCELABLE_WRITE_RETURN_VALUE = 1;
  int describeContents();
  void writeToParcel(Parcel dest, int flags);
  interface Creator<T> {
    T createFromParcel(Parcel source);
    T[] newArray(int size);
  }
  interface ClassLoaderCreator<T> extends Creator<T> {
    T createFromParcel(Parcel source, ClassLoader loader);
  }
}
