package android.app.job;

import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.IBinder;

/** AOSP android.app.job.JobService: the app-side entry point the job scheduler binds to run a job. */
public abstract class JobService extends Service {
  public static final String PERMISSION_BIND = "android.permission.BIND_JOB_SERVICE";

  /** The binder the in-process scheduler receives; it calls the service on the main thread. */
  static final class Engine extends Binder {
    final JobService service;
    Engine(JobService service) { this.service = service; }
  }

  private final Engine engine = new Engine(this);

  @Override public final IBinder onBind(Intent intent) { return engine; }
  public abstract boolean onStartJob(JobParameters params);
  public abstract boolean onStopJob(JobParameters params);
  public void onNetworkChanged(JobParameters params) {}
  public final void jobFinished(JobParameters params, boolean wantsReschedule) {
    LocalJobScheduler.get().finished(params, wantsReschedule);
  }
}
