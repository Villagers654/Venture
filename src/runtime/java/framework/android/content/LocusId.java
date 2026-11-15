package android.content;

import android.os.Parcel;
import android.os.Parcelable;

/** AOSP android.content.LocusId: an app-defined identifier of the content an activity shows. */
public final class LocusId implements Parcelable {
  private final String id;
  public LocusId(String id) {
    if (id == null || id.isEmpty()) throw new IllegalArgumentException("id cannot be empty");
    this.id = id;
  }
  public String getId() { return id; }
  @Override public int hashCode() { return 31 + id.hashCode(); }
  @Override public boolean equals(Object other) {
    return this == other || other instanceof LocusId locus && id.equals(locus.id);
  }
  // AOSP prints the length only, so the identifier does not reach logs.
  @Override public String toString() { return "LocusId[" + id.length() + "_chars]"; }
  public int describeContents() { return 0; }
  public void writeToParcel(Parcel out, int flags) { out.writeString(id); }
  public static final Parcelable.Creator<LocusId> CREATOR = new Parcelable.Creator<>() {
    public LocusId createFromParcel(Parcel in) { return new LocusId(in.readString()); }
    public LocusId[] newArray(int size) { return new LocusId[size]; }
  };
}
