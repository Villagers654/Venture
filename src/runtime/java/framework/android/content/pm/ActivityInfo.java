package android.content.pm;
public class ActivityInfo extends ComponentInfo {
  public static final int SCREEN_ORIENTATION_UNSPECIFIED = -1, SCREEN_ORIENTATION_LANDSCAPE = 0,
      SCREEN_ORIENTATION_PORTRAIT = 1, SCREEN_ORIENTATION_SENSOR = 4,
      SCREEN_ORIENTATION_REVERSE_LANDSCAPE = 8, SCREEN_ORIENTATION_REVERSE_PORTRAIT = 9,
      SCREEN_ORIENTATION_FULL_SENSOR = 10, SCREEN_ORIENTATION_USER = 2,
      SCREEN_ORIENTATION_BEHIND = 3, SCREEN_ORIENTATION_NOSENSOR = 5,
      SCREEN_ORIENTATION_SENSOR_LANDSCAPE = 6, SCREEN_ORIENTATION_SENSOR_PORTRAIT = 7,
      SCREEN_ORIENTATION_USER_LANDSCAPE = 11, SCREEN_ORIENTATION_USER_PORTRAIT = 12,
      SCREEN_ORIENTATION_FULL_USER = 13, SCREEN_ORIENTATION_LOCKED = 14;
  public int theme, screenOrientation = -1, configChanges;
  public String parentActivityName;
  public String targetActivity;
  public int getThemeResource() {
    return theme != 0 ? theme : applicationInfo.theme;
  }
}
