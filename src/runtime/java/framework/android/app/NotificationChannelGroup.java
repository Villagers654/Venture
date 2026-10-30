package android.app;

import android.os.Parcel;
import android.os.Parcelable;
import java.util.ArrayList;
import java.util.List;

/** AOSP android.app.NotificationChannelGroup: a named set of channels. */
public final class NotificationChannelGroup implements Parcelable {
  private final String id;
  private final CharSequence name;
  private String description;
  final List<NotificationChannel> channels = new ArrayList<>();

  public NotificationChannelGroup(String id, CharSequence name) {
    this.id = id;
    this.name = name;
  }
  public String getId() { return id; }
  public CharSequence getName() { return name; }
  public String getDescription() { return description; }
  public void setDescription(String description) { this.description = description; }
  public List<NotificationChannel> getChannels() { return new ArrayList<>(channels); }
  public boolean isBlocked() { return false; }
  public int describeContents() { return 0; }
  public void writeToParcel(Parcel dest, int flags) { dest.writeParcelable(this, flags); }
  public static final Creator<NotificationChannelGroup> CREATOR = new Creator<>() {
    public NotificationChannelGroup createFromParcel(Parcel in) { return in.readParcelable(null); }
    public NotificationChannelGroup[] newArray(int size) { return new NotificationChannelGroup[size]; }
  };
}
