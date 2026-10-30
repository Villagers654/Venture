package android.app.job;

import android.net.Network;
import android.net.Uri;
import android.os.PersistableBundle;

/** AOSP android.app.job.JobParameters: one execution of a scheduled job. */
public class JobParameters {
  public static final int STOP_REASON_UNDEFINED = 0, STOP_REASON_CANCELLED_BY_APP = 1, STOP_REASON_PREEMPT = 2,
                          STOP_REASON_TIMEOUT = 3, STOP_REASON_DEVICE_STATE = 4, STOP_REASON_CONSTRAINT_BATTERY_NOT_LOW = 5,
                          STOP_REASON_CONSTRAINT_CHARGING = 6, STOP_REASON_CONSTRAINT_CONNECTIVITY = 7,
                          STOP_REASON_CONSTRAINT_DEVICE_IDLE = 8, STOP_REASON_CONSTRAINT_STORAGE_NOT_LOW = 9,
                          STOP_REASON_QUOTA = 10, STOP_REASON_BACKGROUND_RESTRICTION = 11, STOP_REASON_APP_STANDBY = 12,
                          STOP_REASON_USER = 13, STOP_REASON_SYSTEM_PROCESSING = 14, STOP_REASON_ESTIMATED_APP_LAUNCH_TIME_CHANGED = 15;

  private final JobInfo job;
  private final boolean overrideDeadlineExpired;
  private final Network network;
  volatile int stopReason = STOP_REASON_UNDEFINED;

  JobParameters(JobInfo job, boolean overrideDeadlineExpired, Network network) {
    this.job = job;
    this.overrideDeadlineExpired = overrideDeadlineExpired;
    this.network = network;
  }

  public int getJobId() { return job.getId(); }
  public PersistableBundle getExtras() { return job.getExtras(); }
  public android.os.Bundle getTransientExtras() { return new android.os.Bundle(); }
  public boolean isOverrideDeadlineExpired() { return overrideDeadlineExpired; }
  public boolean isExpeditedJob() { return job.isExpedited(); }
  public int getStopReason() { return stopReason; }
  public Uri[] getTriggeredContentUris() { return null; }
  public String[] getTriggeredContentAuthorities() { return null; }
  public Network getNetwork() { return network; }
  JobInfo job() { return job; }
}
