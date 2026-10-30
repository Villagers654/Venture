package android.app;

import android.media.AudioAttributes;
import android.net.Uri;
import android.os.Parcel;
import android.os.Parcelable;

/** AOSP android.app.NotificationChannel: a category of notifications with its alert settings. */
public final class NotificationChannel implements Parcelable {
  public static final String DEFAULT_CHANNEL_ID = "miscellaneous";
  private final String id;
  private CharSequence name;
  private String description, group, conversationId, parentId;
  private int importance, lockscreenVisibility = NotificationManager.VISIBILITY_NO_OVERRIDE, lightColor;
  private boolean bypassDnd, showBadge = true, lights, vibration, allowBubbles, blockable;
  private Uri sound = android.provider.Settings.System.DEFAULT_NOTIFICATION_URI;
  private AudioAttributes audioAttributes = Notification.AUDIO_ATTRIBUTES_DEFAULT;
  private long[] vibrationPattern;

  public NotificationChannel(String id, CharSequence name, int importance) {
    this.id = id;
    this.name = name;
    this.importance = importance;
  }
  public String getId() { return id; }
  public CharSequence getName() { return name; }
  public void setName(CharSequence name) { this.name = name; }
  public String getDescription() { return description; }
  public void setDescription(String description) { this.description = description; }
  public int getImportance() { return importance; }
  public void setImportance(int importance) { this.importance = importance; }
  public String getGroup() { return group; }
  public void setGroup(String groupId) { group = groupId; }
  public boolean canBypassDnd() { return bypassDnd; }
  public void setBypassDnd(boolean bypass) { bypassDnd = bypass; }
  public int getLockscreenVisibility() { return lockscreenVisibility; }
  public void setLockscreenVisibility(int visibility) { lockscreenVisibility = visibility; }
  public boolean canShowBadge() { return showBadge; }
  public void setShowBadge(boolean show) { showBadge = show; }
  public Uri getSound() { return sound; }
  public AudioAttributes getAudioAttributes() { return audioAttributes; }
  public void setSound(Uri sound, AudioAttributes attributes) { this.sound = sound; audioAttributes = attributes; }
  public boolean shouldShowLights() { return lights; }
  public void enableLights(boolean on) { lights = on; }
  public int getLightColor() { return lightColor; }
  public void setLightColor(int argb) { lightColor = argb; }
  public boolean shouldVibrate() { return vibration; }
  public void enableVibration(boolean on) { vibration = on; }
  public long[] getVibrationPattern() { return vibrationPattern; }
  public void setVibrationPattern(long[] pattern) {
    vibration = pattern != null && pattern.length > 0;
    vibrationPattern = pattern;
  }
  public boolean canBubble() { return allowBubbles; }
  public void setAllowBubbles(boolean allow) { allowBubbles = allow; }
  public boolean isBlockable() { return blockable; }
  public void setBlockable(boolean blockable) { this.blockable = blockable; }
  public String getConversationId() { return conversationId; }
  public String getParentChannelId() { return parentId; }
  public void setConversationId(String parentChannelId, String conversationId) {
    parentId = parentChannelId;
    this.conversationId = conversationId;
  }
  public boolean isImportantConversation() { return false; }
  public boolean hasUserSetImportance() { return false; }
  public boolean hasUserSetSound() { return false; }
  public int describeContents() { return 0; }
  public void writeToParcel(Parcel dest, int flags) { dest.writeParcelable(this, flags); }
  public static final Creator<NotificationChannel> CREATOR = new Creator<>() {
    public NotificationChannel createFromParcel(Parcel in) { return in.readParcelable(null); }
    public NotificationChannel[] newArray(int size) { return new NotificationChannel[size]; }
  };
}
