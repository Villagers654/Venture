package android.app.usage;

import java.util.ArrayList;
import java.util.List;

/** Usage access (PACKAGE_USAGE_STATS) is never granted to a game, so queries return no stats as on a device. */
public final class UsageStatsManager {
  public static final int INTERVAL_DAILY = 0, INTERVAL_WEEKLY = 1, INTERVAL_MONTHLY = 2, INTERVAL_YEARLY = 3,
                          INTERVAL_BEST = 4;
  public List<UsageStats> queryUsageStats(int intervalType, long beginTime, long endTime) { return new ArrayList<>(); }
  public java.util.Map<String, UsageStats> queryAndAggregateUsageStats(long beginTime, long endTime) {
    return new java.util.HashMap<>();
  }
  public boolean isAppInactive(String packageName) { return false; }
  public int getAppStandbyBucket() { return 10; }
}
