package android.app.job;

import android.content.Context;
import java.util.List;

/** AOSP android.app.job.JobScheduler. Context.getSystemService returns the in-process scheduler. */
public abstract class JobScheduler {
  public static final int RESULT_FAILURE = 0, RESULT_SUCCESS = 1;

  public JobScheduler() {}
  public static JobScheduler ventureLocal(Context context) {
    LocalJobScheduler scheduler = LocalJobScheduler.get();
    scheduler.attach(context);
    return scheduler;
  }

  public abstract int schedule(JobInfo job);
  public abstract void cancel(int jobId);
  public abstract void cancelAll();
  public abstract List<JobInfo> getAllPendingJobs();
  public abstract JobInfo getPendingJob(int jobId);
}
