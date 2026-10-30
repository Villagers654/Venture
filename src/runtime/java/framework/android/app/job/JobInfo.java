package android.app.job;

import android.content.ComponentName;
import android.net.NetworkRequest;
import android.net.Uri;
import android.os.PersistableBundle;
import java.util.ArrayList;
import java.util.List;

/** AOSP android.app.job.JobInfo: what a job runs and when it may run. */
public class JobInfo {
  public static final int NETWORK_TYPE_NONE = 0, NETWORK_TYPE_ANY = 1, NETWORK_TYPE_UNMETERED = 2,
                          NETWORK_TYPE_NOT_ROAMING = 3, NETWORK_TYPE_CELLULAR = 4;
  public static final int BACKOFF_POLICY_LINEAR = 0, BACKOFF_POLICY_EXPONENTIAL = 1;
  public static final long DEFAULT_INITIAL_BACKOFF_MILLIS = 30_000L;
  public static final long MAX_BACKOFF_DELAY_MILLIS = 5 * 60 * 60 * 1000L;
  public static final int PRIORITY_MIN = 100, PRIORITY_LOW = 200, PRIORITY_DEFAULT = 300, PRIORITY_HIGH = 400,
                          PRIORITY_MAX = 500;

  /** AOSP JobInfo.TriggerContentUri: a content URI whose changes trigger the job. */
  public static final class TriggerContentUri {
    public static final int FLAG_NOTIFY_FOR_DESCENDANTS = 1;
    private final Uri uri;
    private final int flags;
    public TriggerContentUri(Uri uri, int flags) {
      if (uri == null) throw new NullPointerException("uri must not be null");
      this.uri = uri;
      this.flags = flags;
    }
    public Uri getUri() { return uri; }
    public int getFlags() { return flags; }
  }

  private final int id;
  private final ComponentName service;
  private final PersistableBundle extras;
  private final int networkType;
  private final NetworkRequest network;
  private final boolean charging, batteryNotLow, idle, storageNotLow, persisted, expedited, importantWhileForeground;
  private final long minLatency, maxExecutionDelay, intervalMillis, flexMillis, initialBackoff;
  private final int backoffPolicy, priority;
  private final boolean periodic, hasEarlyConstraint, hasLateConstraint;
  private final TriggerContentUri[] triggerContentUris;
  private final long triggerContentUpdateDelay, triggerContentMaxDelay;

  private JobInfo(Builder b) {
    id = b.id;
    service = b.service;
    extras = b.extras;
    networkType = b.networkType;
    network = b.network;
    charging = b.charging;
    batteryNotLow = b.batteryNotLow;
    idle = b.idle;
    storageNotLow = b.storageNotLow;
    persisted = b.persisted;
    expedited = b.expedited;
    importantWhileForeground = b.importantWhileForeground;
    minLatency = b.minLatency;
    maxExecutionDelay = b.maxExecutionDelay;
    intervalMillis = b.intervalMillis;
    flexMillis = b.flexMillis;
    initialBackoff = b.initialBackoff;
    backoffPolicy = b.backoffPolicy;
    priority = b.priority;
    periodic = b.periodic;
    hasEarlyConstraint = b.hasEarlyConstraint;
    hasLateConstraint = b.hasLateConstraint;
    triggerContentUris = b.triggerContentUris.isEmpty() ? null : b.triggerContentUris.toArray(new TriggerContentUri[0]);
    triggerContentUpdateDelay = b.triggerContentUpdateDelay;
    triggerContentMaxDelay = b.triggerContentMaxDelay;
  }

  public int getId() { return id; }
  public ComponentName getService() { return service; }
  public PersistableBundle getExtras() { return extras; }
  public int getNetworkType() { return networkType; }
  public NetworkRequest getRequiredNetwork() { return network; }
  public boolean isRequireCharging() { return charging; }
  public boolean isRequireBatteryNotLow() { return batteryNotLow; }
  public boolean isRequireDeviceIdle() { return idle; }
  public boolean isRequireStorageNotLow() { return storageNotLow; }
  public boolean isPersisted() { return persisted; }
  public boolean isExpedited() { return expedited; }
  public boolean isImportantWhileForeground() { return importantWhileForeground; }
  public long getMinLatencyMillis() { return minLatency; }
  public long getMaxExecutionDelayMillis() { return maxExecutionDelay; }
  public boolean isPeriodic() { return periodic; }
  public long getIntervalMillis() { return intervalMillis; }
  public long getFlexMillis() { return flexMillis; }
  public long getInitialBackoffMillis() { return initialBackoff; }
  public int getBackoffPolicy() { return backoffPolicy; }
  public int getPriority() { return priority; }
  public TriggerContentUri[] getTriggerContentUris() { return triggerContentUris; }
  public long getTriggerContentUpdateDelay() { return triggerContentUpdateDelay; }
  public long getTriggerContentMaxDelay() { return triggerContentMaxDelay; }
  public static final long getMinPeriodMillis() { return 15 * 60 * 1000L; }
  public static final long getMinFlexMillis() { return 5 * 60 * 1000L; }

  public static final class Builder {
    private final int id;
    private final ComponentName service;
    private PersistableBundle extras = new PersistableBundle();
    private int networkType = NETWORK_TYPE_NONE;
    private NetworkRequest network;
    private boolean charging, batteryNotLow, idle, storageNotLow, persisted, expedited, importantWhileForeground;
    private long minLatency, maxExecutionDelay, intervalMillis, flexMillis;
    private long initialBackoff = DEFAULT_INITIAL_BACKOFF_MILLIS;
    private int backoffPolicy = BACKOFF_POLICY_EXPONENTIAL, priority = PRIORITY_DEFAULT;
    private boolean periodic, hasEarlyConstraint, hasLateConstraint, explicitTiming;
    private final List<TriggerContentUri> triggerContentUris = new ArrayList<>();
    private long triggerContentUpdateDelay = -1, triggerContentMaxDelay = -1;

    public Builder(int jobId, ComponentName jobService) {
      id = jobId;
      service = jobService;
    }
    public Builder setExtras(PersistableBundle extras) { this.extras = extras; return this; }
    public Builder setRequiredNetworkType(int networkType) { this.networkType = networkType; return this; }
    public Builder setRequiredNetwork(NetworkRequest networkRequest) {
      network = networkRequest;
      if (networkRequest != null && networkType == NETWORK_TYPE_NONE) networkType = NETWORK_TYPE_ANY;
      if (networkRequest == null) networkType = NETWORK_TYPE_NONE;
      return this;
    }
    public Builder setRequiresCharging(boolean requiresCharging) { charging = requiresCharging; return this; }
    public Builder setRequiresBatteryNotLow(boolean v) { batteryNotLow = v; return this; }
    public Builder setRequiresDeviceIdle(boolean v) { idle = v; return this; }
    public Builder setRequiresStorageNotLow(boolean v) { storageNotLow = v; return this; }
    public Builder setPersisted(boolean v) { persisted = v; return this; }
    public Builder setExpedited(boolean v) { expedited = v; return this; }
    public Builder setImportantWhileForeground(boolean v) { importantWhileForeground = v; return this; }
    public Builder setPriority(int v) { priority = v; return this; }
    public Builder setMinimumLatency(long minLatencyMillis) {
      minLatency = minLatencyMillis;
      hasEarlyConstraint = explicitTiming = true;
      return this;
    }
    public Builder setOverrideDeadline(long maxExecutionDelayMillis) {
      maxExecutionDelay = maxExecutionDelayMillis;
      hasLateConstraint = explicitTiming = true;
      return this;
    }
    public Builder setPeriodic(long intervalMillis) { return setPeriodic(intervalMillis, intervalMillis); }
    public Builder setPeriodic(long intervalMillis, long flexMillis) {
      this.intervalMillis = Math.max(intervalMillis, getMinPeriodMillis());
      this.flexMillis = Math.max(Math.min(flexMillis, this.intervalMillis), Math.max(this.intervalMillis * 5 / 100, getMinFlexMillis()));
      periodic = hasEarlyConstraint = hasLateConstraint = true;
      return this;
    }
    public Builder setBackoffCriteria(long initialBackoffMillis, int backoffPolicy) {
      initialBackoff = Math.max(initialBackoffMillis, 10_000L);
      this.backoffPolicy = backoffPolicy;
      return this;
    }
    public Builder addTriggerContentUri(TriggerContentUri uri) { triggerContentUris.add(uri); return this; }
    public Builder setTriggerContentUpdateDelay(long durationMs) { triggerContentUpdateDelay = durationMs; return this; }
    public Builder setTriggerContentMaxDelay(long durationMs) { triggerContentMaxDelay = durationMs; return this; }
    public JobInfo build() {
      if (service == null) throw new IllegalArgumentException("You're trying to build a job with no service component");
      if (periodic && explicitTiming)
        throw new IllegalArgumentException("Can't call setMinimumLatency() or setOverrideDeadline() on a periodic job.");
      if (periodic && !triggerContentUris.isEmpty())
        throw new IllegalArgumentException("Can't call addTriggerContentUri() on a periodic job");
      if (persisted && !triggerContentUris.isEmpty())
        throw new IllegalArgumentException("Can't call addTriggerContentUri() on a persisted job");
      if (expedited && (periodic || minLatency != 0 || idle || charging || batteryNotLow || storageNotLow))
        throw new IllegalArgumentException("An expedited job can't be periodic, delayed, or depend on idle, charging, "
            + "battery or storage");
      return new JobInfo(this);
    }
  }
}
