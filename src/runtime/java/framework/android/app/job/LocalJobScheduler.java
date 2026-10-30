package android.app.job;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.net.ConnectivityManager;
import android.net.Network;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * The job scheduler of the one hosted process, in the process itself: a job runs through its
 * JobService, bound like the system binds it, once its delay has passed and its constraints hold.
 * A PC is always charging, never low on battery or storage, and not idle while a game runs, so
 * idle-only jobs wait for their deadline. Content-triggered jobs see no content changes. Jobs do
 * not outlive the process; apps reschedule persisted work on the next start, as after a reboot.
 */
final class LocalJobScheduler extends JobScheduler {
  private static final String TAG = "JobScheduler";
  private static final long NETWORK_RECHECK_MILLIS = 30_000L;
  private static final LocalJobScheduler INSTANCE = new LocalJobScheduler();

  private final class Entry implements ServiceConnection, Runnable {
    final JobInfo job;
    final long scheduledAt = SystemClock.uptimeMillis();
    long earliest, deadline;
    int failures;
    JobParameters running;
    JobService service;
    boolean bound;

    Entry(JobInfo job) { this.job = job; }
    // Due: the delay (or periodic window) has passed.
    @Override public void run() {
      if (jobs.get(job.getId()) != this || running != null) return;
      long now = SystemClock.uptimeMillis();
      boolean deadlineExpired = deadline > 0 && now >= deadline;
      Network network = null;
      if (job.getNetworkType() != JobInfo.NETWORK_TYPE_NONE) {
        network = connectivity().getActiveNetwork();
        if (network == null && !deadlineExpired) {
          handler.postAtTime(this, this, now + NETWORK_RECHECK_MILLIS);
          return;
        }
      }
      if ((job.isRequireDeviceIdle() || job.getTriggerContentUris() != null) && !deadlineExpired) {
        if (deadline > 0) handler.postAtTime(this, this, deadline);
        return;
      }
      running = new JobParameters(job, deadlineExpired, network);
      Intent intent = new Intent().setComponent(job.getService());
      bound = context.bindService(intent, this, Context.BIND_AUTO_CREATE);
      if (!bound) {
        Log.w(TAG, "Job " + job.getId() + " service " + job.getService().flattenToShortString() + " is unavailable");
        running = null;
        jobs.remove(job.getId(), this);
      }
    }
    @Override public void onServiceConnected(ComponentName name, IBinder binder) {
      if (!(binder instanceof JobService.Engine engine) || running == null) {
        Log.w(TAG, "Job " + job.getId() + " service " + name.flattenToShortString() + " is not a JobService");
        stop(this);
        return;
      }
      service = engine.service;
      JobParameters params = running;
      if (!service.onStartJob(params) && running == params) finish(this, false);
    }
    @Override public void onServiceDisconnected(ComponentName name) { service = null; }
  }

  private final Map<Integer, Entry> jobs = new LinkedHashMap<>();
  private final Handler handler = new Handler(Looper.getMainLooper());
  private Context context;
  private ConnectivityManager connectivity;

  static LocalJobScheduler get() { return INSTANCE; }
  synchronized void attach(Context context) {
    if (this.context == null) this.context = context.getApplicationContext() != null ? context.getApplicationContext() : context;
  }
  private ConnectivityManager connectivity() {
    if (connectivity == null) connectivity = (ConnectivityManager) context.getSystemService(Context.CONNECTIVITY_SERVICE);
    return connectivity;
  }

  @Override public int schedule(JobInfo job) {
    ComponentName service = job.getService();
    if (!context.getPackageName().equals(service.getPackageName()))
      throw new IllegalArgumentException("Cannot schedule job in another package: " + service.getPackageName());
    synchronized (this) {
      Entry entry = new Entry(job);
      long now = entry.scheduledAt;
      if (job.isPeriodic()) {
        entry.earliest = now + job.getIntervalMillis() - job.getFlexMillis();
        entry.deadline = now + job.getIntervalMillis();
      } else {
        entry.earliest = now + job.getMinLatencyMillis();
        entry.deadline = job.getMaxExecutionDelayMillis() > 0 ? now + job.getMaxExecutionDelayMillis() : 0;
      }
      Entry previous = jobs.put(job.getId(), entry);
      handler.post(() -> {
        if (previous != null) stop(previous, JobParameters.STOP_REASON_CANCELLED_BY_APP);
        handler.postAtTime(entry, entry, entry.earliest);
      });
    }
    return RESULT_SUCCESS;
  }
  @Override public void cancel(int jobId) {
    Entry entry;
    synchronized (this) { entry = jobs.remove(jobId); }
    if (entry != null) handler.post(() -> stop(entry, JobParameters.STOP_REASON_CANCELLED_BY_APP));
  }
  @Override public void cancelAll() {
    List<Entry> all;
    synchronized (this) {
      all = new ArrayList<>(jobs.values());
      jobs.clear();
    }
    handler.post(() -> {
      for (Entry entry : all) stop(entry, JobParameters.STOP_REASON_CANCELLED_BY_APP);
    });
  }
  @Override public synchronized List<JobInfo> getAllPendingJobs() {
    var out = new ArrayList<JobInfo>();
    for (Entry entry : jobs.values()) out.add(entry.job);
    return out;
  }
  @Override public synchronized JobInfo getPendingJob(int jobId) {
    Entry entry = jobs.get(jobId);
    return entry == null ? null : entry.job;
  }

  void finished(JobParameters params, boolean reschedule) {
    handler.post(() -> {
      Entry entry;
      synchronized (this) { entry = jobs.get(params.getJobId()); }
      if (entry != null && entry.running == params) finish(entry, reschedule);
    });
  }
  // On the main thread: the run ended, so the job is rescheduled with backoff, repeats, or is done.
  private void finish(Entry entry, boolean reschedule) {
    stop(entry);
    long now = SystemClock.uptimeMillis();
    JobInfo job = entry.job;
    synchronized (this) {
      if (jobs.get(job.getId()) != entry) return;
      if (reschedule) {
        entry.failures++;
        long backoff = job.getBackoffPolicy() == JobInfo.BACKOFF_POLICY_LINEAR
            ? job.getInitialBackoffMillis() * entry.failures
            : job.getInitialBackoffMillis() << Math.min(entry.failures - 1, 20);
        entry.earliest = now + Math.min(backoff, JobInfo.MAX_BACKOFF_DELAY_MILLIS);
        entry.deadline = 0;
      } else if (job.isPeriodic()) {
        entry.failures = 0;
        entry.earliest = now + job.getIntervalMillis() - job.getFlexMillis();
        entry.deadline = now + job.getIntervalMillis();
      } else {
        jobs.remove(job.getId());
        return;
      }
    }
    handler.postAtTime(entry, entry, entry.earliest);
  }
  // On the main thread: a running job is told to stop, then unbound.
  private void stop(Entry entry, int reason) {
    handler.removeCallbacksAndMessages(entry);
    JobParameters params = entry.running;
    if (params != null && entry.service != null) {
      params.stopReason = reason;
      entry.service.onStopJob(params);
    }
    stop(entry);
  }
  private void stop(Entry entry) {
    handler.removeCallbacksAndMessages(entry);
    entry.running = null;
    entry.service = null;
    if (entry.bound) {
      entry.bound = false;
      context.unbindService(entry);
    }
  }
}
