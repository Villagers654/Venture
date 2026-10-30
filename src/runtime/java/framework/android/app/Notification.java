package android.app;

import android.content.Context;
import android.graphics.drawable.Icon;
import android.media.AudioAttributes;
import android.os.*;

/** Notification value objects, posted through NotificationManager. */
public class Notification implements Parcelable, Cloneable {
  public static final int FLAG_SHOW_LIGHTS = 1, FLAG_ONGOING_EVENT = 2, FLAG_INSISTENT = 4,
      FLAG_ONLY_ALERT_ONCE = 8, FLAG_AUTO_CANCEL = 16, FLAG_NO_CLEAR = 32, FLAG_FOREGROUND_SERVICE = 64,
      FLAG_HIGH_PRIORITY = 128, FLAG_LOCAL_ONLY = 256, FLAG_GROUP_SUMMARY = 512, FLAG_BUBBLE = 4096;
  public static final int DEFAULT_SOUND = 1, DEFAULT_VIBRATE = 2, DEFAULT_LIGHTS = 4, DEFAULT_ALL = -1;
  public static final int PRIORITY_MIN = -2, PRIORITY_LOW = -1, PRIORITY_DEFAULT = 0, PRIORITY_HIGH = 1, PRIORITY_MAX = 2;
  public static final int VISIBILITY_SECRET = -1, VISIBILITY_PRIVATE = 0, VISIBILITY_PUBLIC = 1;
  public static final int STREAM_DEFAULT = -1, BADGE_ICON_NONE = 0, BADGE_ICON_SMALL = 1, BADGE_ICON_LARGE = 2,
      GROUP_ALERT_ALL = 0, GROUP_ALERT_SUMMARY = 1, GROUP_ALERT_CHILDREN = 2,
      FOREGROUND_SERVICE_DEFAULT = 0, FOREGROUND_SERVICE_IMMEDIATE = 1, FOREGROUND_SERVICE_DEFERRED = 2;
  public static final AudioAttributes AUDIO_ATTRIBUTES_DEFAULT = new AudioAttributes.Builder()
      .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION).setUsage(AudioAttributes.USAGE_NOTIFICATION).build();
  public int icon, iconLevel, number, flags, defaults, priority, visibility, color;
  public long when = System.currentTimeMillis();
  public CharSequence tickerText;
  public PendingIntent contentIntent, deleteIntent, fullScreenIntent;
  public android.net.Uri sound;
  public long[] vibrate;
  public Bundle extras = new Bundle();
  public AudioAttributes audioAttributes = AUDIO_ATTRIBUTES_DEFAULT;
  public int audioStreamType = STREAM_DEFAULT, ledARGB, ledOnMS, ledOffMS;
  public Action[] actions;
  public Notification publicVersion;
  private String channelId, category, group, sortKey, shortcutId;
  private Icon smallIcon, largeIcon;
  private int badgeIcon, groupAlertBehavior, foregroundServiceBehavior;
  private long timeoutAfter;
  private CharSequence settingsText;
  public Notification() {}
  public Notification(int icon, CharSequence tickerText, long when) {
    this.icon = icon; this.tickerText = tickerText; this.when = when;
  }
  public String getChannelId() { return channelId; }
  public String getCategory() { return category; }
  public String getGroup() { return group; }
  public String getSortKey() { return sortKey; }
  public String getShortcutId() { return shortcutId; }
  public Icon getSmallIcon() { return smallIcon; }
  public Icon getLargeIcon() { return largeIcon; }
  public int getBadgeIconType() { return badgeIcon; }
  public int getGroupAlertBehavior() { return groupAlertBehavior; }
  public long getTimeoutAfter() { return timeoutAfter; }
  public CharSequence getSettingsText() { return settingsText; }
  public Notification clone() {
    try {
      var copy = (Notification) super.clone();
      copy.extras = new Bundle(extras);
      copy.vibrate = vibrate == null ? null : vibrate.clone();
      copy.actions = actions == null ? null : actions.clone();
      return copy;
    } catch (CloneNotSupportedException impossible) { throw new AssertionError(impossible); }
  }
  public int describeContents() { return 0; }
  public void writeToParcel(Parcel out, int flags) { out.writeParcelable(this, flags); }

  // AOSP Notification.Action: a button with an icon, a title and the PendingIntent it sends.
  public static class Action implements Parcelable {
    public static final int SEMANTIC_ACTION_NONE = 0, SEMANTIC_ACTION_REPLY = 1, SEMANTIC_ACTION_MARK_AS_READ = 2,
        SEMANTIC_ACTION_MARK_AS_UNREAD = 3, SEMANTIC_ACTION_DELETE = 4, SEMANTIC_ACTION_ARCHIVE = 5,
        SEMANTIC_ACTION_MUTE = 6, SEMANTIC_ACTION_UNMUTE = 7, SEMANTIC_ACTION_THUMBS_UP = 8,
        SEMANTIC_ACTION_THUMBS_DOWN = 9, SEMANTIC_ACTION_CALL = 10;
    public int icon;
    public CharSequence title;
    public PendingIntent actionIntent;
    private final Icon iconRef;
    private final Bundle extras;
    private final boolean allowGeneratedReplies, contextual, authenticationRequired;
    private final int semanticAction;
    @Deprecated public Action(int icon, CharSequence title, PendingIntent intent) {
      this(icon, null, title, intent, new Bundle(), true, SEMANTIC_ACTION_NONE, false, false);
    }
    private Action(int icon, Icon iconRef, CharSequence title, PendingIntent intent, Bundle extras,
        boolean allowGeneratedReplies, int semanticAction, boolean contextual, boolean authenticationRequired) {
      this.icon = icon; this.iconRef = iconRef; this.title = title; this.actionIntent = intent; this.extras = extras;
      this.allowGeneratedReplies = allowGeneratedReplies; this.semanticAction = semanticAction;
      this.contextual = contextual; this.authenticationRequired = authenticationRequired;
    }
    public Icon getIcon() {
      return iconRef == null && icon != 0 ? Icon.createWithResource("", icon) : iconRef;
    }
    public Bundle getExtras() { return extras; }
    public boolean getAllowGeneratedReplies() { return allowGeneratedReplies; }
    public int getSemanticAction() { return semanticAction; }
    public boolean isContextual() { return contextual; }
    public boolean isAuthenticationRequired() { return authenticationRequired; }
    public Action clone() {
      return new Action(icon, iconRef, title, actionIntent, new Bundle(extras), allowGeneratedReplies,
          semanticAction, contextual, authenticationRequired);
    }
    public int describeContents() { return 0; }
    public void writeToParcel(Parcel out, int flags) { out.writeParcelable(this, flags); }
    public static final Creator<Action> CREATOR = new Creator<>() {
      public Action createFromParcel(Parcel p) { return p.readParcelable(null); }
      public Action[] newArray(int size) { return new Action[size]; }
    };
    public static final class Builder {
      private final int icon;
      private final Icon iconRef;
      private final CharSequence title;
      private final PendingIntent intent;
      private final Bundle extras = new Bundle();
      private boolean allowGeneratedReplies = true, contextual, authenticationRequired;
      private int semanticAction;
      @Deprecated public Builder(int icon, CharSequence title, PendingIntent intent) {
        this.icon = icon; this.iconRef = null; this.title = title; this.intent = intent;
      }
      public Builder(Icon icon, CharSequence title, PendingIntent intent) {
        this.icon = icon != null && icon.getType() == Icon.TYPE_RESOURCE ? icon.getResId() : 0;
        this.iconRef = icon; this.title = title; this.intent = intent;
      }
      public Builder(Action action) {
        this(action.getIcon(), action.title, action.actionIntent);
        extras.putAll(action.extras);
        allowGeneratedReplies = action.allowGeneratedReplies; semanticAction = action.semanticAction;
        contextual = action.contextual; authenticationRequired = action.authenticationRequired;
      }
      public Builder addExtras(Bundle extras) { if (extras != null) this.extras.putAll(extras); return this; }
      public Bundle getExtras() { return extras; }
      public Builder setAllowGeneratedReplies(boolean allow) { allowGeneratedReplies = allow; return this; }
      public Builder setSemanticAction(int action) { semanticAction = action; return this; }
      public Builder setContextual(boolean contextual) { this.contextual = contextual; return this; }
      public Builder setAuthenticationRequired(boolean required) { authenticationRequired = required; return this; }
      public Action build() {
        if (contextual && intent == null) throw new NullPointerException("Contextual Actions must contain a valid PendingIntent");
        return new Action(icon, iconRef, title, intent, new Bundle(extras), allowGeneratedReplies, semanticAction,
            contextual, authenticationRequired);
      }
    }
  }
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
    private final java.util.List<Action> actions = new java.util.ArrayList<>();
    private Style style;
    public Builder(Context context) {}
    public Builder(Context context, String channelId) { value.channelId = channelId; }
    public Builder setWhen(long when) { value.when = when; return this; }
    public Builder setShowWhen(boolean show) { value.extras.putBoolean("android.showWhen", show); return this; }
    public Builder setSmallIcon(int icon) { value.icon = icon; return this; }
    public Builder setSmallIcon(int icon, int level) { value.icon = icon; value.iconLevel = level; return this; }
    public Builder setSmallIcon(Icon icon) {
      value.smallIcon = icon;
      if (icon != null && icon.getType() == Icon.TYPE_RESOURCE) value.icon = icon.getResId();
      return this;
    }
    public Builder setLargeIcon(Icon icon) { value.largeIcon = icon; return this; }
    public Builder setLargeIcon(android.graphics.Bitmap bitmap) {
      return setLargeIcon(bitmap == null ? null : Icon.createWithBitmap(bitmap));
    }
    public Builder setContentTitle(CharSequence title) { value.extras.putCharSequence("android.title", title); return this; }
    public Builder setContentText(CharSequence text) { value.extras.putCharSequence("android.text", text); return this; }
    public Builder setSubText(CharSequence text) { value.extras.putCharSequence("android.subText", text); return this; }
    public Builder setContentInfo(CharSequence text) { value.extras.putCharSequence("android.infoText", text); return this; }
    public Builder setSettingsText(CharSequence text) { value.settingsText = text; return this; }
    public Builder setTicker(CharSequence text) { value.tickerText = text; return this; }
    public Builder setContentIntent(PendingIntent intent) { value.contentIntent = intent; return this; }
    public Builder setDeleteIntent(PendingIntent intent) { value.deleteIntent = intent; return this; }
    public Builder setFullScreenIntent(PendingIntent intent, boolean highPriority) {
      value.fullScreenIntent = intent;
      return flag(FLAG_HIGH_PRIORITY, highPriority);
    }
    public Builder setDefaults(int defaults) { value.defaults = defaults; return this; }
    public Builder setPriority(int priority) { value.priority = priority; return this; }
    public Builder setVisibility(int visibility) { value.visibility = visibility; return this; }
    public Builder setPublicVersion(Notification n) { value.publicVersion = n == null ? null : n.clone(); return this; }
    public Builder setColor(int color) { value.color = color; return this; }
    public Builder setColorized(boolean on) { value.extras.putBoolean("android.colorized", on); return this; }
    public Builder setNumber(int number) { value.number = number; return this; }
    public Builder setSound(android.net.Uri sound) { value.sound = sound; return this; }
    public Builder setSound(android.net.Uri sound, int streamType) {
      value.sound = sound;
      value.audioStreamType = streamType;
      return this;
    }
    public Builder setSound(android.net.Uri sound, AudioAttributes attributes) {
      value.sound = sound;
      value.audioAttributes = attributes != null ? attributes : AUDIO_ATTRIBUTES_DEFAULT;
      return this;
    }
    public Builder setVibrate(long[] pattern) { value.vibrate = pattern == null ? null : pattern.clone(); return this; }
    public Builder setLights(int argb, int onMs, int offMs) {
      value.ledARGB = argb; value.ledOnMS = onMs; value.ledOffMS = offMs;
      return this;
    }
    public Builder setProgress(int max, int progress, boolean indeterminate) {
      value.extras.putInt("android.progress", progress);
      value.extras.putInt("android.progressMax", max);
      value.extras.putBoolean("android.progressIndeterminate", indeterminate);
      return this;
    }
    public Builder setUsesChronometer(boolean on) { value.extras.putBoolean("android.showChronometer", on); return this; }
    public Builder setChronometerCountDown(boolean on) { value.extras.putBoolean("android.chronometerCountDown", on); return this; }
    public Builder setCategory(String category) { value.category = category; return this; }
    public Builder setGroup(String group) { value.group = group; return this; }
    public Builder setSortKey(String key) { value.sortKey = key; return this; }
    public Builder setShortcutId(String id) { value.shortcutId = id; return this; }
    public Builder setTimeoutAfter(long durationMs) { value.timeoutAfter = durationMs; return this; }
    public Builder setBadgeIconType(int icon) { value.badgeIcon = icon; return this; }
    public Builder setGroupAlertBehavior(int behavior) { value.groupAlertBehavior = behavior; return this; }
    public Builder setForegroundServiceBehavior(int behavior) { value.foregroundServiceBehavior = behavior; return this; }
    public Builder setAllowSystemGeneratedContextualActions(boolean allowed) { return this; }
    public Builder setRemoteInputHistory(CharSequence[] text) {
      value.extras.putCharSequenceArray("android.remoteInputHistory", text);
      return this;
    }
    public Builder addPerson(String uri) {
      var people = value.extras.getStringArrayList("android.people");
      if (people == null) value.extras.putStringArrayList("android.people", people = new java.util.ArrayList<>());
      people.add(uri);
      return this;
    }
    public Builder addAction(Action action) {
      if (action != null) actions.add(action);
      return this;
    }
    public Builder addAction(int icon, CharSequence title, PendingIntent intent) {
      return addAction(new Action(icon, title, intent));
    }
    public Builder setActions(Action... list) {
      actions.clear();
      if (list != null) for (Action a : list) if (a != null) actions.add(a);
      return this;
    }
    private Builder flag(int flag, boolean on) { value.flags = on ? value.flags | flag : value.flags & ~flag; return this; }
    public Builder setOngoing(boolean on) { return flag(FLAG_ONGOING_EVENT, on); }
    public Builder setAutoCancel(boolean on) { return flag(FLAG_AUTO_CANCEL, on); }
    public Builder setOnlyAlertOnce(boolean on) { return flag(FLAG_ONLY_ALERT_ONCE, on); }
    public Builder setLocalOnly(boolean on) { return flag(FLAG_LOCAL_ONLY, on); }
    public Builder setGroupSummary(boolean on) { return flag(FLAG_GROUP_SUMMARY, on); }
    public Builder setStyle(Style style) { if (this.style != style) { this.style = style; if (style != null) style.setBuilder(this); } return this; }
    public Builder setExtras(Bundle extras) { value.extras = extras == null ? new Bundle() : new Bundle(extras); return this; }
    public Builder addExtras(Bundle extras) { if (extras != null) value.extras.putAll(extras); return this; }
    public Bundle getExtras() { return value.extras; }
    public Notification build() {
      var copy = value.clone();
      copy.actions = actions.isEmpty() ? null : actions.toArray(new Action[0]);
      if (style != null) style.apply(copy.extras);
      return copy;
    }
    public Notification getNotification() { return build(); }
  }
}
