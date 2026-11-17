package android.app.usage;

public final class UsageStats {
  private final String packageName;
  UsageStats(String packageName) { this.packageName = packageName; }
  public String getPackageName() { return packageName; }
  public long getFirstTimeStamp() { return 0; }
  public long getLastTimeStamp() { return 0; }
  public long getLastTimeUsed() { return 0; }
  public long getTotalTimeInForeground() { return 0; }
}
