package android.media;

/** A Quest has no ringtones: no default sound is set for any type. */
public class RingtoneManager {
  public static final int TYPE_RINGTONE = 1, TYPE_NOTIFICATION = 2, TYPE_ALARM = 4, TYPE_ALL = 7;
  public RingtoneManager(android.content.Context context) {}
  public android.net.Uri getRingtoneUri(int position) { return null; }
  public int getRingtonePosition(android.net.Uri uri) { return -1; }
  public static android.net.Uri getActualDefaultRingtoneUri(android.content.Context context, int type) { return null; }
  public static android.net.Uri getDefaultUri(int type) {
    return switch (type) {
      case TYPE_RINGTONE -> android.provider.Settings.System.DEFAULT_RINGTONE_URI;
      case TYPE_NOTIFICATION -> android.provider.Settings.System.DEFAULT_NOTIFICATION_URI;
      case TYPE_ALARM -> android.provider.Settings.System.DEFAULT_ALARM_ALERT_URI;
      default -> null;
    };
  }
}
