package android.app;

import android.content.Context;
import android.os.*;

/** Notification value objects. No host notification service is advertised. */
public class Notification implements Parcelable, Cloneable {
  public static final int FLAG_SHOW_LIGHTS = 1, FLAG_ONGOING_EVENT = 2, FLAG_INSISTENT = 4,
      FLAG_ONLY_ALERT_ONCE = 8, FLAG_AUTO_CANCEL = 16, FLAG_NO_CLEAR = 32;
  public static final int DEFAULT_SOUND = 1, DEFAULT_VIBRATE = 2, DEFAULT_LIGHTS = 4, DEFAULT_ALL = -1;
  public int icon, iconLevel, number, flags, defaults, priority, visibility, color;
  public long when = System.currentTimeMillis();
  public CharSequence tickerText;
  public PendingIntent contentIntent, deleteIntent, fullScreenIntent;
  public android.net.Uri sound;
  public long[] vibrate;
  public Bundle extras = new Bundle();
  private String channelId, category, group, sortKey;
  public Notification() {}
  public Notification(int icon, CharSequence tickerText, long when) {
    this.icon = icon; this.tickerText = tickerText; this.when = when;
  }
  public String getChannelId() { return channelId; }
  public String getCategory() { return category; }
  public String getGroup() { return group; }
  public String getSortKey() { return sortKey; }
  public Notification clone() {
    try {
      var copy = (Notification) super.clone();
      copy.extras = new Bundle(extras);
      copy.vibrate = vibrate == null ? null : vibrate.clone();
      return copy;
    } catch (CloneNotSupportedException impossible) { throw new AssertionError(impossible); }
  }
  public int describeContents() { return 0; }
  public void writeToParcel(Parcel out, int flags) { out.writeParcelable(this, flags); }
  public abstract static class Style {
    protected Builder mBuilder;
    protected CharSequence bigContentTitle, summaryText;
    public void setBuilder(Builder builder) {
      if (mBuilder != builder) { mBuilder = builder; if (builder != null) builder.setStyle(this); }
    }
    public Notification build() { return mBuilder == null ? null : mBuilder.build(); }
    protected void apply(Bundle extras) {
      extras.putCharSequence("android.title.big", bigContentTitle);
      extras.putCharSequence("android.summaryText", summaryText);
    }
  }
  public static class BigTextStyle extends Style {
    private CharSequence text;
    public BigTextStyle() {}
    public BigTextStyle(Builder builder) { setBuilder(builder); }
    public BigTextStyle bigText(CharSequence text) { this.text = text; return this; }
    public BigTextStyle setBigContentTitle(CharSequence title) { bigContentTitle = title; return this; }
    public BigTextStyle setSummaryText(CharSequence summary) { summaryText = summary; return this; }
    protected void apply(Bundle extras) { super.apply(extras); extras.putCharSequence("android.bigText", text); }
  }
  public static class Builder {
    private final Notification value = new Notification();
    private Style style;
    public Builder(Context context) {}
    public Builder(Context context, String channelId) { value.channelId = channelId; }
    public Builder setWhen(long when) { value.when = when; return this; }
    public Builder setShowWhen(boolean show) { value.extras.putBoolean("android.showWhen", show); return this; }
    public Builder setSmallIcon(int icon) { value.icon = icon; return this; }
    public Builder setSmallIcon(int icon, int level) { value.icon = icon; value.iconLevel = level; return this; }
    public Builder setContentTitle(CharSequence title) { value.extras.putCharSequence("android.title", title); return this; }
    public Builder setContentText(CharSequence text) { value.extras.putCharSequence("android.text", text); return this; }
    public Builder setSubText(CharSequence text) { value.extras.putCharSequence("android.subText", text); return this; }
    public Builder setContentInfo(CharSequence text) { value.extras.putCharSequence("android.infoText", text); return this; }
    public Builder setTicker(CharSequence text) { value.tickerText = text; return this; }
    public Builder setContentIntent(PendingIntent intent) { value.contentIntent = intent; return this; }
    public Builder setDeleteIntent(PendingIntent intent) { value.deleteIntent = intent; return this; }
    public Builder setDefaults(int defaults) { value.defaults = defaults; return this; }
    public Builder setPriority(int priority) { value.priority = priority; return this; }
    public Builder setVisibility(int visibility) { value.visibility = visibility; return this; }
    public Builder setColor(int color) { value.color = color; return this; }
    public Builder setNumber(int number) { value.number = number; return this; }
    public Builder setSound(android.net.Uri sound) { value.sound = sound; return this; }
    public Builder setVibrate(long[] pattern) { value.vibrate = pattern == null ? null : pattern.clone(); return this; }
    public Builder setCategory(String category) { value.category = category; return this; }
    public Builder setGroup(String group) { value.group = group; return this; }
    public Builder setSortKey(String key) { value.sortKey = key; return this; }
    private Builder flag(int flag, boolean on) { value.flags = on ? value.flags | flag : value.flags & ~flag; return this; }
    public Builder setOngoing(boolean on) { return flag(FLAG_ONGOING_EVENT, on); }
    public Builder setAutoCancel(boolean on) { return flag(FLAG_AUTO_CANCEL, on); }
    public Builder setOnlyAlertOnce(boolean on) { return flag(FLAG_ONLY_ALERT_ONCE, on); }
    public Builder setStyle(Style style) { if (this.style != style) { this.style = style; if (style != null) style.setBuilder(this); } return this; }
    public Builder setExtras(Bundle extras) { value.extras = extras == null ? new Bundle() : new Bundle(extras); return this; }
    public Notification build() { var copy = value.clone(); if (style != null) style.apply(copy.extras); return copy; }
    public Notification getNotification() { return build(); }
  }
}
