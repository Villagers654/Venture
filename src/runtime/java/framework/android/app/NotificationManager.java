package android.app;

import android.content.Context;
import android.os.Build;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * AOSP android.app.NotificationManager for the one hosted process. Channels and posted
 * notifications are kept as the system keeps them for the app; the headset's notification
 * shade has no counterpart on the PC, so nothing is shown.
 */
public class NotificationManager {
  public static final int IMPORTANCE_UNSPECIFIED = -1000, IMPORTANCE_NONE = 0, IMPORTANCE_MIN = 1,
      IMPORTANCE_LOW = 2, IMPORTANCE_DEFAULT = 3, IMPORTANCE_HIGH = 4, IMPORTANCE_MAX = 5;
  public static final int VISIBILITY_NO_OVERRIDE = -1000;
  public static final int INTERRUPTION_FILTER_UNKNOWN = 0, INTERRUPTION_FILTER_ALL = 1,
      INTERRUPTION_FILTER_PRIORITY = 2, INTERRUPTION_FILTER_NONE = 3, INTERRUPTION_FILTER_ALARMS = 4;
  public static final String ACTION_NOTIFICATION_POLICY_ACCESS_GRANTED_CHANGED =
      "android.app.action.NOTIFICATION_POLICY_ACCESS_GRANTED_CHANGED";
  public static final String ACTION_APP_BLOCK_STATE_CHANGED = "android.app.action.APP_BLOCK_STATE_CHANGED";

  private record Key(String tag, int id) {}
  private static final Map<String, NotificationChannel> channels = new LinkedHashMap<>();
  private static final Map<String, NotificationChannelGroup> groups = new LinkedHashMap<>();
  private static final Map<Key, Notification> posted = new LinkedHashMap<>();

  public NotificationManager() {}
  public NotificationManager(Context context) {}

  public void notify(int id, Notification notification) { notify(null, id, notification); }
  public void notify(String tag, int id, Notification notification) {
    if (notification == null) throw new NullPointerException("notification");
    if (Build.VERSION.SDK_INT >= 26 && notification.getChannelId() != null) {
      synchronized (channels) {
        if (!channels.containsKey(notification.getChannelId()))
          android.util.Log.e("NotificationService", "No Channel found for pkg=" + pkg() + ", channelId="
              + notification.getChannelId() + ", id=" + id + ", tag=" + tag);
      }
    }
    synchronized (posted) { posted.put(new Key(tag, id), notification.clone()); }
  }
  public void cancel(int id) { cancel(null, id); }
  public void cancel(String tag, int id) { synchronized (posted) { posted.remove(new Key(tag, id)); } }
  public void cancelAll() { synchronized (posted) { posted.clear(); } }

  public void createNotificationChannel(NotificationChannel channel) {
    if (channel == null) throw new NullPointerException("channel");
    synchronized (channels) {
      // The app may update the name and description of an existing channel, and lower its importance.
      NotificationChannel existing = channels.get(channel.getId());
      if (existing != null) {
        existing.setName(channel.getName());
        existing.setDescription(channel.getDescription());
        if (channel.getImportance() < existing.getImportance()) existing.setImportance(channel.getImportance());
        if (existing.getGroup() == null) existing.setGroup(channel.getGroup());
        return;
      }
      channels.put(channel.getId(), channel);
    }
  }
  public void createNotificationChannels(List<NotificationChannel> list) {
    for (NotificationChannel channel : list) createNotificationChannel(channel);
  }
  public NotificationChannel getNotificationChannel(String channelId) {
    synchronized (channels) { return channels.get(channelId); }
  }
  public NotificationChannel getNotificationChannel(String channelId, String conversationId) {
    return getNotificationChannel(channelId);
  }
  public List<NotificationChannel> getNotificationChannels() {
    synchronized (channels) { return new ArrayList<>(channels.values()); }
  }
  public void deleteNotificationChannel(String channelId) {
    if (NotificationChannel.DEFAULT_CHANNEL_ID.equals(channelId))
      throw new IllegalArgumentException("Cannot delete default channel");
    synchronized (channels) { channels.remove(channelId); }
  }
  public void createNotificationChannelGroup(NotificationChannelGroup group) {
    synchronized (channels) { groups.put(group.getId(), group); }
  }
  public void createNotificationChannelGroups(List<NotificationChannelGroup> list) {
    for (NotificationChannelGroup group : list) createNotificationChannelGroup(group);
  }
  public NotificationChannelGroup getNotificationChannelGroup(String groupId) {
    synchronized (channels) { return withChannels(groups.get(groupId)); }
  }
  public List<NotificationChannelGroup> getNotificationChannelGroups() {
    synchronized (channels) {
      var out = new ArrayList<NotificationChannelGroup>();
      for (NotificationChannelGroup group : groups.values()) out.add(withChannels(group));
      return out;
    }
  }
  private static NotificationChannelGroup withChannels(NotificationChannelGroup group) {
    if (group == null) return null;
    group.channels.clear();
    for (NotificationChannel channel : channels.values())
      if (group.getId().equals(channel.getGroup())) group.channels.add(channel);
    return group;
  }
  public void deleteNotificationChannelGroup(String groupId) {
    synchronized (channels) {
      groups.remove(groupId);
      channels.values().removeIf(channel -> groupId.equals(channel.getGroup()));
    }
  }
  public boolean areNotificationsEnabled() { return true; }
  public boolean areNotificationsPaused() { return false; }
  public boolean areBubblesAllowed() { return false; }
  public boolean areBubblesEnabled() { return false; }
  public int getImportance() { return IMPORTANCE_DEFAULT; }
  public boolean isNotificationPolicyAccessGranted() { return false; }
  public int getCurrentInterruptionFilter() { return INTERRUPTION_FILTER_ALL; }
  public boolean canNotifyAsPackage(String pkg) { return pkg().equals(pkg); }
  private static String pkg() { return com.venture.runtime.RuntimeEnvironment.get().manifest.info.packageName; }
}
